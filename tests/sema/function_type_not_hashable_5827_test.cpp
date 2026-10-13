// fix #5827 (§3.6 clause (e), §27.7.1, v0.338; proposal 0161 B2): a function
// type is not **hashable**. `Set<(int) -> int>`, `Dictionary<(int) -> int, V>`
// and `where T: hashable` at a function type are compile errors at the
// instantiation site. The sema predicate `isEqualityBearing` answered `true`
// for every non-named type, so the hash-key check accepted them.
//
// Spec lines (docs/Bezel_Language_Spec.md, v0.338):
//   §3.6 hashable row, clause (e) (line 484): "(e) a function type
//     ([§27.7.1]: a lambda equals only itself, so a hash container of function
//     values would hold every lambda evaluation apart; ...)".
//   §27.7.1 (line 6011): "A function type is nonetheless not hashable
//     ([§3.6], clause (e)) ...: a Set<(int) -> int>, a Dictionary<(int) -> int,
//     V>, stdlib §5's hashable block and where T: hashable at a function type
//     are rejected at the instantiation site, and a collection of callbacks is
//     an Array<(int) -> int>, whose Contains and Remove compare by this
//     equality".
//   §3.6 clause (c): T? over a non-hashable T is not hashable.
//
// Controls (must stay accepted): Array/Stack Contains, Array.IndexOf and `=`
// on function values (equality, §27.7.1), a function type as a Dictionary
// VALUE, and Set<int>.

// NOTE (#5967): the accept controls below run on runAot, not runAotChecked.
// A method-reference function value that is copied or stored into a collection
// leaks its 48 B function object on the BASE compiler too (LSan, exit 99), so
// runAotChecked reports ran=false ("sanitizer fired") independent of #5827.
// Re-port them to runAotChecked when #5967 lands.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── T1 — §3.6 clause (e): `Set<(int) -> int>` is rejected at the annotation.
BEZEL_TEST(sema_function_type_not_hashable_5827,
           set_of_function_type_rejected_spec_3_6_clause_e) {
    auto r = runAot(
        "module M5827A; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var f: (int) -> int := Demo.Twice; "
        "    using var s := new Set<(int) -> int>(); "
        "    s.Add(f); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not **hashable**"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "clause (e)"));
}

// ── T2 — §3.6 clause (e): `Dictionary<(int) -> int, int>` is rejected.
BEZEL_TEST(sema_function_type_not_hashable_5827,
           dictionary_function_key_rejected_spec_3_6_clause_e) {
    auto r = runAot(
        "module M5827B; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var f: (int) -> int := Demo.Twice; "
        "    using var d := new Dictionary<(int) -> int, int>(); "
        "    d.Add(f, 1); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "clause (e)"));
}

// ── T3 — §3.6 clause (c) over clause (e): a nullable function-type key
// (`((int) -> int)?`) is not hashable either.
BEZEL_TEST(sema_function_type_not_hashable_5827,
           nullable_function_set_element_rejected_spec_3_6_clause_c_e) {
    auto r = runAot(
        "module M5827C; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    using var s := new Set<((int) -> int)?>(); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "clause (e)"));
}

// ── T4 — §27.7.1 control: `Array<(int) -> int>.Contains` stays admitted
// (the issue's and proposal 0161 §8's "callback collection" case). Exit code
// 1 when the element is found.
BEZEL_TEST(sema_function_type_not_hashable_5827,
           array_function_type_contains_stays_accepted_spec_27_7_1) {
    auto r = runAot(
        "module M5827D; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run(): int "
        "    var f: (int) -> int := Demo.Twice; "
        "    var a := new Array<(int) -> int>(); "
        "    a.Add(f); "
        "    if a.Contains(f) then return 1; end if; "
        "    return 0; "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return Demo.Run(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}

// ── T6 — control: a non-function hashable key (`Set<int>`) stays accepted.
BEZEL_TEST(sema_function_type_not_hashable_5827,
           set_of_int_still_accepted_control) {
    auto r = runAot(
        "module M5827F; "
        "static class Demo "
        "  public static method Run(): int "
        "    using var s := new Set<int>(); "
        "    s.Add(3); "
        "    return s.Count; "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return Demo.Run(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           where_hashable_class_at_function_type_spec_14_2_and_3_6_clause_e) {
    auto r = runAot(
        "module M5827G; "
        "class H<T> where T: hashable end H; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var f: (int) -> int := Demo.Twice; "
        "    var h := new H<(int) -> int>(); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`where T: hashable`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "function type"));
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           where_hashable_class_at_nullable_function_type_spec_14_2_and_3_6_clause_c_e) {
    auto r = runAot(
        "module M5827H; "
        "class H<T> where T: hashable end H; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var f: (int) -> int := Demo.Twice; "
        "    var h := new H<((int) -> int)?>(); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`where T: hashable`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "function type"));
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           where_hashable_method_at_function_type_spec_14_2_and_3_6_clause_e) {
    auto r = runAot(
        "module M5827I; "
        "static class G public static method Hold<T>(x: T) where T: hashable end Hold; end G; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var f: (int) -> int := Demo.Twice; "
        "    G.Hold<(int) -> int>(f); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`where T: hashable`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "function type"));
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           where_hashable_method_inferred_function_type_spec_14_2_and_3_6_clause_e) {
    auto r = runAot(
        "module M5827J; "
        "static class G public static method Hold<T>(x: T) where T: hashable end Hold; end G; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var f: (int) -> int := Demo.Twice; "
        "    G.Hold(f); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`where T: hashable`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "function type"));
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           to_set_over_function_array_stdlib_5_hashable_block) {
    auto r = runAot(
        "module M5827K; "
        ""
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var f: (int) -> int := Demo.Twice; "
        "    var a := new Array<(int) -> int>(); a.Add(f); using var s := a.ToSet(); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "requires `T: hashable`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "function type"));
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           distinct_over_function_array_stdlib_5_hashable_block) {
    auto r = runAot(
        "module M5827L; "
        ""
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var f: (int) -> int := Demo.Twice; "
        "    var a := new Array<(int) -> int>(); a.Add(f); var s := a.Distinct(); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "requires `T: hashable`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "function type"));
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           enumerable_contains_over_function_sequence_stdlib_5_hashable_block) {
    auto r = runAot(
        "module M5827N; "
        ""
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run() "
        "    var f: (int) -> int := Demo.Twice; "
        "    var a := new Array<(int) -> int>(); a.Add(f); var e: IEnumerable<(int) -> int> := a; var b := e.Contains(f); "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "requires `T: hashable`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "function type"));
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           dictionary_function_value_accepted_spec_3_6_value_position) {
    auto r = runAot(
        "module M5827O; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run(): int "
        "    var f: (int) -> int := Demo.Twice; "
        "    using var d := new Dictionary<int, (int) -> int>(); d.Add(1, f); return d.Count; "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return Demo.Run(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           array_function_index_of_and_equality_accepted_spec_27_7_1) {
    auto r = runAot(
        "module M5827P; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run(): int "
        "    var f: (int) -> int := Demo.Twice; "
        "    var g: (int) -> int := Demo.Twice; var a := new Array<(int) -> int>(); a.Add(f); if (f = g) and (a.IndexOf(f) = 0) then return 1; end if; return 0; "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return Demo.Run(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}

BEZEL_TEST(sema_function_type_not_hashable_5827,
           stack_function_contains_accepted_spec_27_7_1) {
    auto r = runAot(
        "module M5827Q; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Run(): int "
        "    var f: (int) -> int := Demo.Twice; "
        "    using var s := new Stack<(int) -> int>(); s.Push(f); if s.Contains(f) then return 1; end if; return 0; "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return Demo.Run(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}

// §27.7.1 / §14.2: a generic body's unconstrained `items.Contains(x)` at a
// function-type instantiation is the native equality search (first kind), not
// the §5 hashable block: accepted, and found/not-found are correct. runAot for
// the #5967 reason above.
BEZEL_TEST(sema_function_type_not_hashable_5827,
           generic_bag_contains_at_function_type_accepted_spec_27_7_1) {
    auto r = runAot(
        "module M5827R; "
        "class Bag<T> "
        "  private owned items: Array<T>; "
        "  public constructor() items := new Array<T>(); end constructor; "
        "  public method Put(x: T) items.Add(x); end Put; "
        "  public method Has(x: T): bool => items.Contains(x); "
        "end Bag; "
        "static class Demo "
        "  public static method Twice(n: int): int => n * 2; "
        "  public static method Thrice(n: int): int => n * 3; "
        "  public static method Run(): int "
        "    var f: (int) -> int := Demo.Twice; "
        "    var o: (int) -> int := Demo.Thrice; "
        "    using var b := new Bag<(int) -> int>(); "
        "    b.Put(f); "
        "    var n := 0; "
        "    if b.Has(f) then n := n + 1; end if; "
        "    if b.Has(o) then n := n + 10; end if; "
        "    return n; "
        "  end Run; "
        "end Demo; "
        "class C public static method Main(): int return Demo.Run(); end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 1);
}
