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
