// Spec-derived regression tests for issue #7991 — the §12.7 narrowing of an
// `Object` carrier to a STDLIB CLASS.
//
// THE RULE. Spec §12.7 (v0.350), "Type tests: `is`, `is not`, `as`":
//
//   "`is T` tests whether a value is an instance of `T` and produces `bool`.
//   When the test succeeds and the tested expression is one of [§7.4]'s
//   narrowing carriers, the carrier is **flow-narrowed** to `T` within the
//   controlled block"
//
// and its "Narrowing at an `Object` or interface location" paragraph names
// exactly this carrier:
//
//   "When the narrowed carrier's *declared* type is `Object`, an interface, or
//   the nullable form of either (`Object?`, `I?`) and the tested type `T` is a
//   **value type**, that carrier's storage is a [§8.6] box … Narrowing to a
//   *reference* type is unaffected: no box is involved, and writes through the
//   narrowed binding are ordinary reference-type writes."
//
// §8.1 (v0.350) is the root-`Object` surface: "`Object` is a compiler
// intrinsic; method bodies are provided by the runtime. The declaration above
// defines the public surface."
//
// WHAT WAS BROKEN. `ExprTC::narrowFromTypeTest` (src/sema/ExprTypecheck.cpp)
// decides whether an `is T` test records a §12.7 flow-narrowing. Its
// `Kind::Named` arm was a POSITIVE list admitting (a) a USER declaration,
// (b) the five boxable value builtins, and (c) the built-in EXCEPTION classes.
// A STDLIB CLASS (`Stopwatch`, `StringBuilder`, `TaskScope`, `Mutex`, …)
// carries no `TypeSymbol` — `Resolver`'s §2.3 admission "makes the type name
// resolvable to a Named ref with a null declaration" — so it matched NONE of
// the three clauses: NO narrowing was recorded, the carrier kept its DECLARED
// `Object` type inside the controlled block, and the member read that §12.7's
// own spelling makes legal failed to dispatch.
//
// MEASURED on the base (`ad048d70`, this lane's staging tip): the program
// COMPILES (exit 0) and the call is a silent no-op behind three `internal:`
// warnings —
//
//   internal: codegen cannot dispatch member 'Stop' on receiver of type
//     'Object' … (#2477)
//   internal: codegen cannot read member 'IsRunning' on receiver of type
//     'Object' … (#2547)
//   internal: the argument of this `Console.WriteLine` failed to lower to a
//     value, so the whole statement emits nothing … (#3079)
//
// ⇒ the base's symptom is a WRONG RUNTIME OUTCOME behind exit 0, so every
// accept row below pins the PRINTED VALUE, never the exit code.
//
// THE FIX. `narrowFromTypeTest`'s `Kind::Named` arm gains one clause: a
// declaration-less `Kind::Named` whose head names a BUILT-IN CLASS whose
// runtime value is a heap reference (`common::isHeapPointerReferenceBuiltin`)
// is narrowable, when the head is not one of this body's own generic
// parameters (`currentScopeGenericParamNames()`, the same discriminator the
// built-in-exception clause above already uses).
//
// WHY THAT POPULATION, and not "every declaration-less Named": the narrowing
// is a RETYPE of the carrier, so the carrier's storage must be able to hold
// the target. A built-in VALUE type that does NOT box (`IntPtr`, `UIntPtr`,
// `ReadOnlyStringSpan`, `Boolean`, …) has no representation at an `Object`
// location at all. MEASURED on this fix's own first draft, which admitted every
// declaration-less Named: `var o: Object := 5; if o is ReadOnlyStringSpan
// then Console.WriteLine(o.Length); end;` SEGFAULTED the COMPILER
// (LlvmBackend_Exprs.cpp:6827 — a `CreateExtractValue` on the non-aggregate
// `Object` slot). `common::isHeapPointerReferenceBuiltin` is the ONE shared
// table that answers "is this builtin's runtime value a pointer?" — the same
// question codegen's `lowerTypeRef` asks — so it is exactly the population
// whose narrowed read is the same pointer the carrier already held.
//
// ORACLE. Every accept row RUNS the program and asserts EXACT stdout. The
// six "fix" rows are RED on the base (the member is not dispatched, so the
// value is never printed) and GREEN with the fix. The control rows are GREEN
// on BOTH arms — they are the blast radius, and a control that flipped would
// be a regression of this fix.
//
// Cite: spec §12.7, v0.350 (the narrowing, and the reference-type sentence);
// §8.1, v0.350 (the root `Object` surface); §26.5, v0.350 (extensions are
// resolved on the compile-time type of the receiver); §7.4, v0.350 (the
// narrowing carriers).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Runs `program` and returns its stdout, or a marker when it did not run.
// A row that asserts on this marker FAILS, so a rejected or crashed program
// can never be mistaken for a passing one.
std::string stdoutOf(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did not run>");
    return r.stdoutText;
}

// The `StringBuilder` shape: an `Object` carrier, a §12.7 `is` test against a
// STDLIB CLASS, and a member read of the narrowed carrier. `StringBuilder` is
// used (rather than the issue's `Stopwatch`) because its runtime type tag
// (`bz_sb_desc`) makes the `is` test DECIDABLE at an `Object` carrier, so the
// controlled block really runs and the printed value really proves the
// dispatch. `Stopwatch` has no runtime tag at an `Object` carrier (the
// #5059/#4885 gap), so its `is` test is dropped and its block never runs —
// the `Stopwatch` row below therefore uses the `is not` spelling, whose ELSE
// branch is the one §12.7 narrows to the tested type.
constexpr const char* kSbProgram = R"bzl(module M7991;
public class P
  public static method Main(): int
    var o: Object := new StringBuilder();
    if o is StringBuilder then
      Console.WriteLine("in");
      _ := o.Append("hi");
      Console.WriteLine(o.Length);
    end;
    return 0;
  end Main;
end P;
)bzl";

}  // namespace

// ── THE FIX ROW ──────────────────────────────────────────────────────────────
// §12.7: the carrier is flow-narrowed to `T`, so `o.Length` reads the
// `StringBuilder`'s own `Length` (stdlib §3.1: "public readonly property
// Length: int") and the program prints it. On the base the member read is not
// dispatched, so NOTHING after "in" is printed.
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           narrowed_stdlib_class_member_is_dispatched_spec_12_7) {
    BEZEL_EXPECT_EQ(stdoutOf(kSbProgram), std::string("in\n2\n"));
}

// ── THE ISSUE'S OWN REPRO (`Stopwatch`), PROVEN BY THE PRINTED VALUE ───────
// `if o is not Stopwatch then … else … end` — §12.7: "for `is not T`, the
// variable is narrowed to `T` in the else branch". The `else` block calls
// `Stopwatch.Stop()` and reads `Stopwatch.IsRunning` (stdlib §8.1:
// "public readonly property IsRunning: bool; public method Stop();").
// On the base neither is dispatched and the block prints nothing; with the
// fix `Stop()` really runs and the program prints "False".
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           narrowed_stdlib_class_stopwatch_member_is_dispatched_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991W;
import System.Diagnostics;
public class P
  public static method Main(): int
    var o: Object := Stopwatch.StartNew();
    if o is not Stopwatch then
      Console.WriteLine("not");
    else
      o.Stop();
      Console.WriteLine(o.IsRunning);
    end;
    return 0;
  end Main;
end P;
)bzl"),
        std::string("False\n"));
}

// ── THE SAME SHAPE THROUGH A PARAMETER (a §7.4 narrowing carrier) ──────────
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           narrowed_stdlib_class_through_a_parameter_is_dispatched_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991P;
public class P
  public static method Probe(o: Object): int
    if o is StringBuilder then
      _ := o.Append("ab");
      Console.WriteLine(o.Length);
    end;
    return 0;
  end Probe;
  public static method Main(): int
    Console.WriteLine(Probe(new StringBuilder()));
    return 0;
  end Main;
end P;
)bzl"),
        std::string("2\n0\n"));
}

// ── THE `is not T` ELSE BRANCH (§12.7) ──────────────────────────────────────
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           narrowed_stdlib_class_in_the_is_not_else_branch_is_dispatched_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991N;
public class P
  public static method Main(): int
    var o: Object := new StringBuilder();
    if o is not StringBuilder then
      Console.WriteLine("not");
    else
      _ := o.Append("ab");
      Console.WriteLine(o.Length);
    end;
    return 0;
  end Main;
end P;
)bzl"),
        std::string("2\n"));
}

// ── A NULLABLE `Object?` CARRIER (§12.7 names the nullable form) ───────────
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           narrowed_stdlib_class_on_a_nullable_object_carrier_is_dispatched_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991Q;
public class P
  public static method Main(): int
    var o: Object? := new StringBuilder();
    if o is StringBuilder then
      Console.WriteLine("in");
      _ := o.Append("ab");
      Console.WriteLine(o.Length);
    end;
    return 0;
  end Main;
end P;
)bzl"),
        std::string("in\n2\n"));
}

// ── A TYPE ALIAS TO THE STDLIB CLASS (§27.8: an alias is transparent) ──────
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           narrowed_stdlib_class_through_a_type_alias_is_dispatched_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991A;
type SB := StringBuilder;
public class P
  public static method Main(): int
    var o: Object := new StringBuilder();
    if o is SB then
      Console.WriteLine("in");
      _ := o.Append("ab");
      Console.WriteLine(o.Length);
    end;
    return 0;
  end Main;
end P;
)bzl"),
        std::string("in\n2\n"));
}

// ══ CONTROLS — the blast radius. GREEN on BOTH arms. ═══════════════════════

// §12.7 / §7.4: a §12.7-narrowed carrier whose target is a USER class must
// still narrow (the admitting twin of the fix rows — it worked before the fix
// and must keep working).
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           narrowed_user_class_still_narrows_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991U;
public class Base
  public method Hello(): int => 1;
end Base;
public class Derived : Base
  public method Extra(): int => 2;
end Derived;
public class P
  public static method Main(): int
    var o: Object := new Derived();
    if o is Derived then
      Console.WriteLine(o.Extra());
    end;
    return 0;
  end Main;
end P;
)bzl"),
        std::string("2\n"));
}

// #4812 / #3092: a bare TYPE PARAMETER is not a built-in and stays excluded
// from the narrowing.
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           bare_type_parameter_target_is_still_excluded_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991T;
public class Holder<T>
  public method Peek(v: T): int
    if v is T then
      var x: T := v;
      return 1;
    end;
    return 0;
  end Peek;
end Holder;
public class P
  public static method Main(): int
    using var h := new Holder<int>();
    Console.WriteLine(h.Peek(7));
    return 0;
  end Main;
end P;
)bzl"),
        std::string("1\n"));
}

// The discriminator is by NAME, so a type PARAMETER that SHADOWS a stdlib
// class name is the type parameter, not the built-in — the same rule the
// built-in-exception clause of `narrowFromTypeTest` already states ("A type
// PARAMETER that shadows an exception name is not the built-in and stays
// excluded with every other bare `T`").
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           type_parameter_shadowing_a_stdlib_class_name_is_still_excluded_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991S;
public class P
  public static method Probe<StringBuilder>(o: Object, x: StringBuilder): int
    if o is StringBuilder then
      Console.WriteLine("in");
    end;
    return 0;
  end Probe;
  public static method Main(): int
    Console.WriteLine("RAN");
    return 0;
  end Main;
end P;
)bzl"),
        std::string("RAN\n"));
}

// §12.7 "Narrowing at an `Object` or interface location": a VALUE-type
// optional's own `is` test has its own path (`narrowFromNullTest` / the
// primitive arm) and must not drift with this one.
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           value_type_optional_is_test_is_unaffected_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991O;
public class P
  public static method Main(): int
    var n: int? := 5;
    if n is int then
      Console.WriteLine(n);
    end;
    Console.WriteLine("done");
    return 0;
  end Main;
end P;
)bzl"),
        std::string("5\ndone\n"));
}

// §8.6 "Boxing of inline arrays": a move-only value type cannot be boxed, so
// an `Object` location never holds one and the inline-array arm's own
// exclusion must survive.
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           inline_array_target_is_still_excluded_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991I;
public class P
  public static method Main(): int
    var o: Object := 5;
    if o is [3]int then
      Console.WriteLine(o[0]);
    end;
    Console.WriteLine("done");
    return 0;
  end Main;
end P;
)bzl"),
        std::string("done\n"));
}

// A built-in VALUE type that does NOT box must NOT be narrowed to: the
// `Object` slot holds no such value, and the narrowed read would load the
// slot AS the value type. On this fix's first draft (every declaration-less
// Named admitted) this program SEGFAULTED THE COMPILER; with the shipped
// population it compiles and prints exactly what it printed before the fix.
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           non_boxable_builtin_value_type_is_still_not_narrowed_spec_12_7) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991V;
public class P
  public static method Main(): int
    var o: Object := 5;
    if o is ReadOnlyStringSpan then
      Console.WriteLine(o.Length);
    end;
    Console.WriteLine("done");
    return 0;
  end Main;
end P;
)bzl"),
        std::string("done\n"));
}

// §8.1: the five §8.1 members on the ROOT `Object` are still admitted.
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           section_8_1_members_on_the_root_are_still_admitted_spec_8_1) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991R;
public class P
  public static method Main(): int
    var o: Object := 5;
    Console.WriteLine(o.ToString());
    Console.WriteLine(o.Equals(o));
    Console.WriteLine(o.GetHashCode());
    Console.WriteLine(Object.ReferenceEquals(o, o));
    return 0;
  end Main;
end P;
)bzl"),
        std::string("5\nTrue\n5\nTrue\n"));
}

// §26.5: "Extensions are resolved on the **compile-time** type of the
// receiver" — an `extension Object` still resolves, on the root and on the
// nullable form.
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           extension_on_object_still_resolves_spec_26_5) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991E;
extension Object
  public method Tag(): int => 7;
end;
public class P
  public static method Main(): int
    var o: Object := 5;
    var p: Object? := 6;
    Console.WriteLine(o.Tag());
    Console.WriteLine(p?.Tag() ?? 0);
    return 0;
  end Main;
end P;
)bzl"),
        std::string("7\n7\n"));
}

// §8.1: the ROOT `Object` still takes no generic arguments — "a generic
// `Object<T>` is a different name" (§2.2) is about a USER-declared name; the
// root itself stays arity 0.
//
// WHY THE DIAGNOSTIC TEXT IS NOT PINNED HERE. The exact wording of the root's
// rejection is the #7992 sibling lane's to change (its commit `cc8d3170`
// renames "primitive type cannot take generic arguments" to a message that
// names the ROOT — "`Object` is the root type and takes no generic arguments
// (§8.1) …"). Pinning either wording here would make this lane's row FAIL the
// moment that sibling lands. So this row pins the PROPERTY that is this
// lane's to defend — the root takes no generic arguments, and the reference is
// rejected at compile time — and only the substring "generic arguments", which
// BOTH wordings contain. A user's generic `Object<T>` (the #7992 sibling's
// subject) is deliberately NOT exercised here: it is that lane's verdict, not
// this lane's.
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           root_object_still_takes_no_generic_arguments_spec_8_1) {
    auto r = runAot(R"bzl(module M7991G;
public class P
  public static method Main(): int
    var o: Object<int> := 5;
    return 0;
  end Main;
end P;
)bzl");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "generic arguments"));
}

// The INTERFACE, CLASS, TYPE-PARAMETER, ALIAS and NULLABLE receivers are
// untouched: each is a §7.4 carrier whose own member resolution must not
// change.
BEZEL_TEST(sema_builtin_narrow_stdlib_class_7991,
           interface_class_typeparam_alias_and_nullable_receivers_are_untouched_spec_7_4) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M7991M;
public interface IShape
  method Area(): int;
end;
public class Sq : IShape
  public method Area(): int => 4;
  destructor end destructor;
end Sq;
public class C
  public method Hi(): int => 7;
  destructor end destructor;
end C;
type MyObj := Object;
public class P
  public static method Tag<T>(x: T): string => x.ToString();
  public static method Main(): int
    using var s: IShape := new Sq();
    Console.WriteLine(s.Area());
    using var c := new C();
    Console.WriteLine(c.Hi());
    Console.WriteLine(Tag(5));
    var a: MyObj := 5;
    Console.WriteLine(a.ToString());
    var b: Object? := 6;
    Console.WriteLine(b!.ToString());
    return 0;
  end Main;
end P;
)bzl"),
        std::string("4\n7\n5\n5\n6\n"));
}
