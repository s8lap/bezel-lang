// Regression tests for issue #8127 -- a LEGAL program SEGFAULTS: `$"{this}"`
// inside a class reads the receiver object's OWN bytes as a `bz_string`
// descriptor {ptr data, i64 len, i8 width} and FOLLOWS the "data pointer",
// which is the object's first eight bytes.
//
// THE DEFECT, MEASURED on the pre-fix code (staging @ 8d5b8dd3), stdout +
// exit code (ASan is silent on this class, so stdout is the oracle):
//   * `class C { public N: int := 4242; }` + `$"{this}"`  -> SIGSEGV (exit 139)
//   * `class C { public S: string := "hello"; }` + `$"{this}"` -> an EMPTY line
//   * `record R(X: int, Y: string)` + `$"{this}"`         -> an EMPTY line
//   * a class with a `ToString` override + `$"{this}"`    -> SIGSEGV (exit 139)
//   * `class Derived : Base` + `$"{this}"`                 -> SIGSEGV (exit 139)
//   * `Console.WriteLine(this.ToString())` (no override)  -> NOTHING printed,
//     with the #3079 "the argument of this `Console.WriteLine` failed to lower
//     to a value" warning
//
// §8.5 (spec v0.355): "`ToString` is invoked implicitly inside string
// interpolation (§18.5) via virtual dispatch -- for present values", and the
// compiler-generated default for a CLASS is "the type's QUALIFIED NAME" (the
// module-path-qualified rendering stdlib §2.13 defines for `Type.FullName`).
//
// THE CAUSE. `this` / `base` carry NO sema exprType (sema's
// ExprTypecheck_Exprs.cpp: "Slice 3b: IdentifierExpr name lookup. `this` /
// `base` are implicit-meaning identifiers -- leave them unresolved"), so the
// §18.5 hole's rendering dispatch saw no type at all and fell through to its
// "the value is a pointer, so it must be a bz_string" fallback -- reading the
// receiver's own bytes as a string descriptor. The very same class reached
// through a NON-`this` local rendered correctly all along, because a local
// DOES carry a type; the control rows below pin that.
//
// THE FIX. The hole's operand type is recovered from the emitter's own
// receiver state (the enclosing type of the member body -- the same
// "which type's body am I in" lookup `indexReceiverDecl` performs, extended to
// constructors, property/indexer accessors, lambdas, records, enums and
// extension blocks), and the EXISTING rendering dispatch renders it -- so the
// `this` spelling and the equivalent local spelling now agree.
//
// Oracle: runAot (the strict AOT path). stdout is the oracle for the rendered
// text; the exit code carries the crash (139) on the pre-fix code.
#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// §8.5's class default is the type's QUALIFIED name -- the module-path
// qualified rendering stdlib §2.13 defines for `Type.FullName`.
const char* kQualifiedName = "M.C\n";

// The issue's own repro shape: an INT field first, so the receiver's first
// eight bytes are 4242 -- not a pointer. Following them as a `bz_string`'s
// data pointer is the SIGSEGV.
JitResult runIntFirstField() {
    return runAot(
        "module M;\n"
        "public class C\n"
        "  public N: int := 4242;\n"
        "  public method Go(): int\n"
        "    Console.WriteLine($\"{this}\");\n"
        "    return 0;\n"
        "  end Go;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var c := new C();\n"
        "    return c.Go();\n"
        "  end Main;\n"
        "end Program;\n");
}

// The issue's other repro shape: a STRING field first, so the receiver's
// first eight bytes ARE a pointer -- the string's data pointer -- and the hole
// renders the string's BYTES as the receiver's name (an empty line here).
JitResult runStringFirstField() {
    return runAot(
        "module M;\n"
        "public class C\n"
        "  public S: string := \"hello\";\n"
        "  public method Go(): int\n"
        "    Console.WriteLine($\"{this}\");\n"
        "    return 0;\n"
        "  end Go;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var c := new C();\n"
        "    return c.Go();\n"
        "  end Main;\n"
        "end Program;\n");
}

}  // namespace

// ── The issue's own two repros ──────────────────────────────────────────────

// An int field first: the pre-fix code followed 4242 as a pointer -> SIGSEGV.
BEZEL_TEST(interp_this_class_tostring_8127, int_first_field_renders_qualified_name) {
    auto r = runIntFirstField();
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, kQualifiedName);
}

// A string field first: the pre-fix code read the receiver's own bytes as a
// string descriptor and printed an EMPTY line.
BEZEL_TEST(interp_this_class_tostring_8127, string_first_field_renders_qualified_name) {
    auto r = runStringFirstField();
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, kQualifiedName);
}

// A RECORD receiver: §8.5's positional-record default is the field listing
// ("Point(X: 10, Y: 20)"), so the record's `this` renders the listing -- the
// pre-fix code printed an empty line.
BEZEL_TEST(interp_this_class_tostring_8127, record_receiver_renders_field_listing) {
    auto r = runAot(
        "module M;\n"
        "public record R(X: int, Y: string)\n"
        "  public method Go(): int\n"
        "    Console.WriteLine($\"{this}\");\n"
        "    return 0;\n"
        "  end Go;\n"
        "end R;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var r := new R(1, \"a\");\n"
        "    return r.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "R(X: 1, Y: a)\n");
}

// A user `ToString` OVERRIDE replaces §8.5's default (the override wins for
// every consumer, interpolation included). Pre-fix: SIGSEGV.
BEZEL_TEST(interp_this_class_tostring_8127, user_tostring_override_is_used) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public N: int := 4242;\n"
        "  public override method ToString(): string\n"
        "    return \"C!\";\n"
        "  end ToString;\n"
        "  public method Go(): int\n"
        "    Console.WriteLine($\"{this}\");\n"
        "    return 0;\n"
        "  end Go;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var c := new C();\n"
        "    return c.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "C!\n");
}

// A DERIVED class: §8.5's dispatch is VIRTUAL, so the derived type's own
// rendering answers -- the derived type's qualified name when it declares no
// override. Pre-fix: SIGSEGV.
BEZEL_TEST(interp_this_class_tostring_8127, derived_class_renders_derived_name) {
    auto r = runAot(
        "module M;\n"
        "public class Base\n"
        "  public N: int := 4242;\n"
        "end Base;\n"
        "public class Derived : Base\n"
        "  public method Go(): int\n"
        "    Console.WriteLine($\"{this}\");\n"
        "    return 0;\n"
        "  end Go;\n"
        "end Derived;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var d := new Derived();\n"
        "    return d.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "M.Derived\n");
}

// `base` is the SAME OBJECT as `this` (§12.3), so §8.5's virtual dispatch
// answers the runtime type's rendering -- the derived type's qualified name.
// The `base.ToString()` sibling already answered exactly this (the #1106
// block in LlvmBackend_Calls2.cpp), so the two spellings agree.
BEZEL_TEST(interp_this_class_tostring_8127, base_keyword_renders_runtime_type_name) {
    auto r = runAot(
        "module M;\n"
        "public class Base\n"
        "  public N: int := 4242;\n"
        "end Base;\n"
        "public class Derived : Base\n"
        "  public method Go(): int\n"
        "    Console.WriteLine($\"{base}\");\n"
        "    return 0;\n"
        "  end Go;\n"
        "end Derived;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var d := new Derived();\n"
        "    return d.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "M.Derived\n");
}

// A virtual ToString override reaches the `this` hole through §8.5's virtual
// dispatch: a base that declares the override answers it for the derived
// class too, and the derived class's own override wins over the base's.
BEZEL_TEST(interp_this_class_tostring_8127, virtual_override_reaches_this_hole) {
    auto r = runAot(
        "module M;\n"
        "public class Base\n"
        "  public override method ToString(): string\n"
        "    return \"Base!\";\n"
        "  end ToString;\n"
        "end Base;\n"
        "public class Derived : Base\n"
        "  public method Go(): int\n"
        "    Console.WriteLine($\"{this}\");\n"
        "    return 0;\n"
        "  end Go;\n"
        "end Derived;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var d := new Derived();\n"
        "    return d.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "Base!\n");
}

// ── Controls: the same values reached NOT through `this` ───────────────────

// The non-`this` local of the very same class renders the qualified name --
// this row is GREEN on the pre-fix code too (it is the positive control that
// shows the shape is admitted, and that the fix makes the two spellings
// agree rather than inventing a rendering).
BEZEL_TEST(interp_this_class_tostring_8127, non_this_local_control_renders_qualified_name) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public N: int := 4242;\n"
        "  public S: string := \"hello\";\n"
        "  public method Go(): int\n"
        "    var c := new C();\n"
        "    Console.WriteLine($\"{c}\");\n"
        "    return 0;\n"
        "  end Go;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var c := new C();\n"
        "    return c.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, kQualifiedName);
}

// The record reached through a non-`this` local renders the same listing the
// `this` hole now renders (green on the pre-fix code too).
BEZEL_TEST(interp_this_class_tostring_8127, record_non_this_local_control) {
    auto r = runAot(
        "module M;\n"
        "public record R(X: int, Y: string);\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var r := new R(1, \"a\");\n"
        "    Console.WriteLine($\"{r}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "R(X: 1, Y: a)\n");
}

// ── The sibling: `Console.WriteLine(this.ToString())` ───────────────────────

// §8.5's default ToString reached through the EXPLICIT call: pre-fix this
// degraded with the #3079 "failed to lower to a value" warning and printed
// NOTHING AT ALL (not even the line terminator).
BEZEL_TEST(interp_this_class_tostring_8127, this_tostring_call_sibling_prints_the_name) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public N: int := 4242;\n"
        "  public method Go(): int\n"
        "    Console.WriteLine(this.ToString());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var c := new C();\n"
        "    return c.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, kQualifiedName);
}

// The sibling with an OVERRIDE: the override's text (green before and after).
BEZEL_TEST(interp_this_class_tostring_8127, this_tostring_call_sibling_uses_the_override) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public N: int := 4242;\n"
        "  public override method ToString(): string\n"
        "    return \"C!\";\n"
        "  end ToString;\n"
        "  public method Go(): int\n"
        "    Console.WriteLine(this.ToString());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var c := new C();\n"
        "    return c.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "C!\n");
}

// ── The rendered string must not leak ──────────────────────────────────────

// The hole's rendered text is a FRESH string on the override path (the
// §8.5 default is a literal, but the override's result is minted), and the
// §18.5 chain discharges it after the concat borrows its bytes. A 100-iteration
// loop under the ASan/LSan net makes an undischarged render visible; a
// one-shot run would not.
BEZEL_TEST(interp_this_class_tostring_8127, loop_100_rendered_string_does_not_leak) {
    auto r = runAotChecked(
        "module M;\n"
        "public class C\n"
        "  public N: int := 4242;\n"
        "  public override method ToString(): string\n"
        "    return \"C!\";\n"
        "  end ToString;\n"
        "  public method Go(): int\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      Console.WriteLine($\"{this}\");\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    return 0;\n"
        "  end Go;\n"
        "end C;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var c := new C();\n"
        "    return c.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "C!\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// ── fix-forward: `this` inside an ACCESSOR body lowered from ANOTHER type ──
//
// Review r39-rev-8127 (REJECT of c5aae377): an expression-bodied getter is
// INLINED into its caller's function, a block getter is emitted LAZILY from
// inside its first caller's body, and a setter / indexer accessor is inlined
// the same way -- so the receiver-type recovery answered with the CALLER's
// type: wrong text at exit 0, a type-confused virtual call, and SIGSEGVs in
// programs the pre-#8127 base ran. The owner is now recorded when the
// accessor body is opened. §8.5 (v0.355): the hole renders the RECEIVER's
// ToString -- here B's §8.5 class default, its qualified name.

// s12: class B's expression getter `=> $"prop {this}"` read from class A's method. The getter is INLINED
// into A.Go, so the c5aae377 recovery answered with A's type (`prop M.A`); `this` is B's object.
BEZEL_TEST(interp_this_class_tostring_8127, class_getter_read_from_another_class_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public P: string := \"pp\";\n"
        "  public property Name: string\n"
        "    get => $\"prop {this}\";\n"
        "  end Name;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "prop M.B\n");
}

// s01: the s12 shape where the CALLER (A) overrides ToString. Base and c5aae377 both SIGSEGV.
BEZEL_TEST(interp_this_class_tostring_8127, class_getter_read_from_overriding_class_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public P: string := \"pp\";\n"
        "  public property Name: string\n"
        "    get => $\"prop {this}\";\n"
        "  end Name;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "prop M.B\n");
}

// s13: an expression-bodied INDEXER getter of B read from A (c5aae377: `idx M.A`).
BEZEL_TEST(interp_this_class_tostring_8127, indexer_getter_read_from_another_class_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b[0]);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public P: string := \"pp\";\n"
        "  public property [i: int]: string\n"
        "    get => $\"idx {this}\";\n"
        "  end;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "idx M.B\n");
}

// s15: B's getter read from the static `Program.Main` (c5aae377: `prop M.Program`).
BEZEL_TEST(interp_this_class_tostring_8127, class_getter_read_from_static_main_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public property Name: string\n"
        "    get => $\"prop {this}\";\n"
        "  end Name;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "prop M.B\n");
}

// s23: B declares an unrelated `virtual Boom()`; A overrides ToString. c5aae377 dispatched A's ToString slot
// on B's object and printed `prop BOOM` -- a type-confused virtual call. B has no override, so §8.5's
// class default (B's qualified name) is the rendering.
BEZEL_TEST(interp_this_class_tostring_8127, getter_this_is_not_dispatched_through_the_callers_tostring) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public virtual method Boom(): string => \"BOOM\";\n"
        "  public property Name: string => $\"prop {this}\";\n"
        "end B;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "prop M.B\n");
}

// s20: a CHOICE property `=> $"d {this}"` read from a class method whose class overrides ToString.
// Base ran (`d `); c5aae377 SIGSEGVed (the class arm on a choice value). §8.5: a value-like auto-hashable
// choice renders case-shaped, head unqualified.
BEZEL_TEST(interp_this_class_tostring_8127, choice_getter_read_from_overriding_class_renders_the_case) {
    auto r = runAot(
        "module M;\n"
        "public choice S\n"
        "  K(v: int),\n"
        "  Z;\n"
        "  public property Desc: string => $\"d {this}\";\n"
        "end S;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var s := S.K(3);\n"
        "    Console.WriteLine(s.Desc);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "d S.K(v: 3)\n");
}

// s24: the s20 shape without the caller's override (c5aae377: `d M.A`).
BEZEL_TEST(interp_this_class_tostring_8127, choice_getter_read_from_class_renders_the_case) {
    auto r = runAot(
        "module M;\n"
        "public choice S\n"
        "  K(v: int),\n"
        "  Z;\n"
        "  public property Desc: string => $\"d {this}\";\n"
        "end S;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "    var s := S.K(3);\n"
        "    Console.WriteLine(s.Desc);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "d S.K(v: 3)\n");
}

// s26: B's getter read from a RECORD method. Base ran (`prop `); c5aae377 SIGSEGVed (the record arm on a
// class pointer).
BEZEL_TEST(interp_this_class_tostring_8127, class_getter_read_from_record_method_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public property Name: string => $\"prop {this}\";\n"
        "end B;\n"
        "public record R(X: int, Y: string)\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end R;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var r := new R(1, \"a\");\n"
        "    return r.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "prop M.B\n");
}

// s27: B's getter read from an ENUM method (c5aae377: `prop M.E`).
BEZEL_TEST(interp_this_class_tostring_8127, class_getter_read_from_enum_method_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public property Name: string => $\"prop {this}\";\n"
        "end B;\n"
        "public enum E\n"
        "  X;\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end E;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    return E.X.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "prop M.B\n");
}

// the BLOCK-bodied getter twin: a memoised function emitted lazily from inside A.Go.
BEZEL_TEST(interp_this_class_tostring_8127, block_getter_read_from_another_class_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public property Name: string\n"
        "    get => do\n"
        "      return $\"blk {this}\";\n"
        "    end;\n"
        "  end Name;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "blk M.B\n");
}

// the SETTER twin: B's block setter is inlined into A.Go with `this` = B's object.
BEZEL_TEST(interp_this_class_tostring_8127, inlined_setter_from_another_class_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    b.Val := 7;\n"
        "    Console.WriteLine(b.N);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public property Val: int\n"
        "    get => N;\n"
        "    set => do\n"
        "      Console.WriteLine($\"set {this}\");\n"
        "      N := value;\n"
        "    end;\n"
        "  end Val;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "set M.B\n7\n");
}

// nested inlines: B's getter reads C's getter; each `this` is its own owner, and A's own `this`
// after the inlines is A again (A overrides ToString).
BEZEL_TEST(interp_this_class_tostring_8127, nested_inlined_getters_each_render_their_own_owner) {
    auto r = runAot(
        "module M;\n"
        "public class C\n"
        "  public K: int := 9;\n"
        "  public property Tag: string => $\"c {this}\";\n"
        "end C;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public owned Kid: C := new C();\n"
        "  public property Name: string => $\"b {this} / {Kid.Tag} / {this}\";\n"
        "end B;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b.Name);\n"
        "    Console.WriteLine($\"a {this}\");\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "b M.B / c M.C / M.B\na A!\n");
}

// enum getter read from a class method: §8.5 (v0.355) renders an enum by its qualified name; the
// caller's frame previously stood in for `this` (base and c5aae377 printed `d 0`).
BEZEL_TEST(interp_this_class_tostring_8127, enum_getter_read_from_class_renders_qualified_name) {
    auto r = runAot(
        "module M;\n"
        "public enum E\n"
        "  X, Y;\n"
        "  public property Desc: string => $\"d {this}\";\n"
        "end E;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    Console.WriteLine(E.X.Desc);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "d M.E\n");
}

// positional record getter read from a class method: §8.5 (v0.355) field listing; c5aae377 SIGSEGV'd.
BEZEL_TEST(interp_this_class_tostring_8127, record_getter_read_from_class_renders_fields) {
    auto r = runAot(
        "module M;\n"
        "public record R(X: int, Y: string)\n"
        "  public property Desc: string => $\"d {this}\";\n"
        "end R;\n"
        "public class A\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var r := new R(1, \"a\");\n"
        "    Console.WriteLine(r.Desc);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "d R(X: 1, Y: a)\n");
}

// a getter taken as a function value (property thunk) and called from another class.
BEZEL_TEST(interp_this_class_tostring_8127, getter_bound_as_function_value_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    var f: () -> string := b.Name;\n"
        "    Console.WriteLine(f());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public property Name: string => $\"fv {this}\";\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "fv M.B\n");
}

// a block-bodied indexer getter read from another class's method.
BEZEL_TEST(interp_this_class_tostring_8127, block_indexer_getter_from_another_class_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b[3]);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public property [i: int]: string\n"
        "    get => do\n"
        "      return $\"bix {i} {this}\";\n"
        "    end;\n"
        "  end;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "bix 3 M.B\n");
}

// `=> this.ToString()` read from another class: dispatches on B, not on the caller A.
BEZEL_TEST(interp_this_class_tostring_8127, getter_explicit_tostring_call_from_another_class_renders_owner) {
    auto r = runAot(
        "module M;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var b := new B();\n"
        "    Console.WriteLine(b.Name);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class B\n"
        "  public N: int := 4242;\n"
        "  public property Name: string => this.ToString();\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "M.B\n");
}

// choice getter `=> this.ToString()` read from a class: c5aae377 SIGSEGV'd. The rendered text is a
// PRE-EXISTING gap (base prints nothing too; see review r14), so this row pins only that the
// program runs to completion; it does not pin the text.
BEZEL_TEST(interp_this_class_tostring_8127, choice_getter_explicit_tostring_from_class_does_not_crash) {
    auto r = runAot(
        "module M;\n"
        "public choice S\n"
        "  K(v: int),\n"
        "  Z;\n"
        "  public property Desc: string => this.ToString();\n"
        "end S;\n"
        "public class A\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var s := S.K(3);\n"
        "    Console.WriteLine(s.Desc);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// Fix-forward #3 (review r39-rev-8127c): `this` must be typed and LOADED from
// ONE recovery. 6f894e53 typed a generic record / choice accessor's `this` from
// the frame but loaded it from the caller's state (so the hole read a pointer
// as the struct), and pushed no frame at all for an expression-bodied GENERIC
// method inlined by `emitInlineGenericMethod` or a record explicit constructor
// inlined by `buildRecordValue` (so `this` was typed as the CALLER). §8.5
// (v0.355): "For positional records: a representation of fields, e.g.
// \"Point(X: 10, Y: 20)\"" (head unqualified); "For choices that are value-like
// and auto-hashable (§35.7): … \"TypeName.CaseName(field1: value1, …)\"";
// "For body records that do not satisfy auto-equality conditions … the type's
// qualified name". The programs are the review's own probes, verbatim.

// generic `record P<T>` getter `$"p {this}"` read from a NON-generic record's method (review
// r39-rev-8127c q08): 6f894e53 typed the hole as P but left `this` an unloaded pointer -> SIGSEGV.
BEZEL_TEST(interp_this_class_tostring_8127, generic_record_getter_read_from_record_method_renders_fields) {
    auto r = runAot(
        "module M;\n"
        "public record P<T>(X: T, Y: T)\n"
        "  public property Show: string => $\"p {this}\";\n"
        "end P;\n"
        "public record R(K: int)\n"
        "  public method Go(): string\n"
        "    var p := new P<int>(1, 2);\n"
        "    return p.Show;\n"
        "  end Go;\n"
        "end R;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "    var r := new R(5);\n"
        "    Console.WriteLine(r.Go());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "p P(X: 1, Y: 2)\n");
}

// the same getter read from a GENERIC record `W<U>`'s method (review v05): 6f894e53 SIGSEGV.
BEZEL_TEST(interp_this_class_tostring_8127, generic_record_getter_read_from_generic_record_method_renders_fields) {
    auto r = runAot(
        "module M;\n"
        "public record P<T>(X: T, Y: T)\n"
        "  public property Show: string => $\"p {this}\";\n"
        "end P;\n"
        "public record W<U>(Z: U)\n"
        "  public method Go(): string\n"
        "    var p := new P<int>(1, 2);\n"
        "    return p.Show;\n"
        "  end Go;\n"
        "end W;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "    var w := new W<int>(5);\n"
        "    Console.WriteLine(w.Go());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "p P(X: 1, Y: 2)\n");
}

// generic `choice O<T>` getter read from a record method (review v07): 6f894e53 SIGSEGV. §8.5: a
// value-like auto-hashable choice renders `"TypeName.CaseName(field1: value1, …)"`, head unqualified.
BEZEL_TEST(interp_this_class_tostring_8127, generic_choice_getter_read_from_record_method_renders_the_case) {
    auto r = runAot(
        "module M;\n"
        "public choice O<T>\n"
        "  Some(v: T),\n"
        "  None;\n"
        "  public property D: string => $\"d {this}\";\n"
        "end O;\n"
        "public record R(K: int)\n"
        "  public method Go(): string\n"
        "    var o := O<int>.Some(K);\n"
        "    return o.D;\n"
        "  end Go;\n"
        "end R;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "    var r := new R(3);\n"
        "    Console.WriteLine(r.Go());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "d O.Some(v: 3)\n");
}

// an EXPRESSION-bodied generic record method (`emitInlineGenericMethod` inlines it into the caller)
// called from a class that overrides ToString (review w01): c5aae377 and 6f894e53 typed `this` as
// the caller A and dispatched A's override on P's bytes -> SIGSEGV.
BEZEL_TEST(interp_this_class_tostring_8127, generic_record_expr_method_from_overriding_class_renders_fields) {
    auto r = runAot(
        "module M;\n"
        "public record P<T>(X: T, Y: T)\n"
        "  public method Self(): string => $\"s {this}\";\n"
        "end P;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var p := new P<int>(1, 2);\n"
        "    Console.WriteLine(p.Self());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s P(X: 1, Y: 2)\n");
}

// an expression-bodied generic CHOICE method called from a class (review w03): 6f894e53 REJECTED the
// program at compile time (the choice classified at a bare `O` with no type arguments).
BEZEL_TEST(interp_this_class_tostring_8127, generic_choice_expr_method_from_class_renders_the_case) {
    auto r = runAot(
        "module M;\n"
        "public choice O<T>\n"
        "  Some(v: T),\n"
        "  None;\n"
        "  public method D(): string => $\"d {this}\";\n"
        "end O;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var o := O<int>.Some(3);\n"
        "    Console.WriteLine(o.D());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "d O.Some(v: 3)\n");
}

// the generic record method called from another RECORD's method (review w04): 6f894e53 rendered P's
// bytes as the caller `R(K: 1)`, exit 0.
BEZEL_TEST(interp_this_class_tostring_8127, generic_record_expr_method_from_record_method_renders_fields) {
    auto r = runAot(
        "module M;\n"
        "public record P<T>(X: T, Y: T)\n"
        "  public method Self(): string => $\"s {this}\";\n"
        "end P;\n"
        "public record R(K: int)\n"
        "  public method Go(): string\n"
        "    var p := new P<int>(1, 2);\n"
        "    return p.Self();\n"
        "  end Go;\n"
        "end R;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "    var r := new R(5);\n"
        "    Console.WriteLine(r.Go());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s P(X: 1, Y: 2)\n");
}

// the caller's ToString calls its own virtual `Boom()` (review w05): a caller-typed `this` would
// dispatch A's vtable slot on P<string>'s bytes. 6f894e53: SIGSEGV. A string component renders bare
// (as the existing `record_receiver_renders_field_listing` row pins: `R(X: 1, Y: a)`).
BEZEL_TEST(interp_this_class_tostring_8127, generic_record_expr_method_is_not_dispatched_through_callers_virtual) {
    auto r = runAot(
        "module M;\n"
        "public record P<T>(X: T, Y: T)\n"
        "  public method Self(): string => $\"s {this}\";\n"
        "end P;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public virtual method Boom(): string => \"BOOM\";\n"
        "  public override method ToString(): string => this.Boom();\n"
        "  public method Go(): int\n"
        "    var p := new P<string>(\"x\", \"y\");\n"
        "    Console.WriteLine(p.Self());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s P(X: x, Y: y)\n");
}

// called from static `Program.Main` (review w06): 6f894e53 rendered `s M.Program`.
BEZEL_TEST(interp_this_class_tostring_8127, generic_record_expr_method_from_static_main_renders_fields) {
    auto r = runAot(
        "module M;\n"
        "public record P<T>(X: T, Y: T)\n"
        "  public method Self(): string => $\"s {this}\";\n"
        "end P;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int => 0;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var p := new P<int>(1, 2);\n"
        "    Console.WriteLine(p.Self());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s P(X: 1, Y: 2)\n");
}

// called from a class with no override (review v03): 6f894e53 rendered `s M.A`.
BEZEL_TEST(interp_this_class_tostring_8127, generic_record_expr_method_from_class_renders_fields) {
    auto r = runAot(
        "module M;\n"
        "public record P<T>(X: T, Y: T)\n"
        "  public method Self(): string => $\"s {this}\";\n"
        "end P;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "    var p := new P<int>(1, 2);\n"
        "    Console.WriteLine(p.Self());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s P(X: 1, Y: 2)\n");
}

// `$"ctor {this}"` in a body record's explicit constructor, inlined by `buildRecordValue` at
// `new R(…)` from a class that overrides ToString and then from a record method (review y01):
// 6f894e53 SIGSEGV. `R` declares explicit FIELDS, so it does not satisfy the auto-equality
// conditions (§9.2: "an explicit field declaration among them suppresses the auto-generated
// equality members"), and §8.5 renders such a body record as the type's QUALIFIED name.
BEZEL_TEST(interp_this_class_tostring_8127, record_ctor_this_from_overriding_class_and_record_renders_qualified_name) {
    auto r = runAot(
        "module M;\n"
        "public record R\n"
        "  public X: int;\n"
        "  public Y: string;\n"
        "  public constructor(x: int)\n"
        "    X := x;\n"
        "    Y := \"a\";\n"
        "    Console.WriteLine($\"ctor {this}\");\n"
        "  end constructor;\n"
        "end R;\n"
        "public record Q(K: int)\n"
        "  public method Go(): int\n"
        "    var r := new R(K);\n"
        "    return r.X;\n"
        "  end Go;\n"
        "end Q;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public override method ToString(): string => \"A!\";\n"
        "  public method Go(): int\n"
        "    var r := new R(1);\n"
        "    var q := new Q(2);\n"
        "    Console.WriteLine(q.Go());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ctor M.R\nctor M.R\n2\n");
}

// the same constructor reached from a class with no override (review y02): 6f894e53 `ctor M.A`.
BEZEL_TEST(interp_this_class_tostring_8127, record_ctor_this_from_class_renders_qualified_name) {
    auto r = runAot(
        "module M;\n"
        "public record R\n"
        "  public X: int;\n"
        "  public Y: string;\n"
        "  public constructor(x: int)\n"
        "    X := x;\n"
        "    Y := \"a\";\n"
        "    Console.WriteLine($\"ctor {this}\");\n"
        "  end constructor;\n"
        "end R;\n"
        "public record Q(K: int)\n"
        "  public method Go(): int\n"
        "    var r := new R(K);\n"
        "    return r.X;\n"
        "  end Go;\n"
        "end Q;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "    var r := new R(1);\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ctor M.R\n");
}

// the same constructor reached from a RECORD's method (review y03): 6f894e53 rendered the caller
// `ctor Q(K: 2)`.
BEZEL_TEST(interp_this_class_tostring_8127, record_ctor_this_from_record_method_renders_qualified_name) {
    auto r = runAot(
        "module M;\n"
        "public record R\n"
        "  public X: int;\n"
        "  public Y: string;\n"
        "  public constructor(x: int)\n"
        "    X := x;\n"
        "    Y := \"a\";\n"
        "    Console.WriteLine($\"ctor {this}\");\n"
        "  end constructor;\n"
        "end R;\n"
        "public record Q(K: int)\n"
        "  public method Go(): int\n"
        "    var r := new R(K);\n"
        "    return r.X;\n"
        "  end Go;\n"
        "end Q;\n"
        "public class A\n"
        "  public Q: int := 1;\n"
        "  public method Go(): int\n"
        "\n"
        "    var q := new Q(2);\n"
        "    Console.WriteLine(q.Go());\n"
        "    return 0;\n"
        "  end Go;\n"
        "end A;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new A();\n"
        "    return a.Go();\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ctor M.R\n2\n");
}
