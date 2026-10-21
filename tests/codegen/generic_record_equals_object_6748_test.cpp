// Regression test for issue #6748.
//
// §9.2 (v0.340), "Through a type parameter": `a = b` in a generic body with
// `T` a record and `b: Object` runs the record's equality on a `T` payload and
// answers unequal for any other argument without the body running (§8.4,
// §25.2). The crash: the generic specialization read the operand's sema type as
// the bare `T`, never boxed the record, and handed a record struct and an
// `Object` pointer to emitStructEquality (SIGSEGV, rc 139, at compile time).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kHead =
    "module M; "
    "public class Animal end Animal; "
    "record V(X: int) end V; "
    "record W(X: int) end W; "
    "record B(X: int, S: string) end B; "
    "public class H "
    "  public static method Eq<T>(a: T, b: Object): bool => a = b; "
    "  public static method Ne<T>(a: T, b: Object): bool => a <> b; "
    "  public static method EqR<T>(a: Object, b: T): bool => a = b; "
    "  public static method Two<T, U>(a: T, b: U, c: Object): bool => a = c; "
    "end H; ";

std::string run(const std::string& body, bool checked) {
    const std::string src = std::string(kHead) + "public class Program "
        "  public static method Main(): int " + body + " end Main; "
        "end Program;";
    JitResult r = checked ? runAotChecked(src) : runAot(src);
    if (!r.ran || r.exitCode != 0)
        return "<<fail rc=" + std::to_string(r.exitCode) + ">>";
    return r.stdoutText;
}

// Leak-checked: no boxed record operand is minted.
std::string out(const std::string& body) { return run(body, true); }
// runAot, not runAotChecked: a record boxed into an `Object` slot leaks its
// 24 B box independently of this fix (#6769, sibling of #6733); these rows pin
// the printed answers.
std::string outU(const std::string& body) { return run(body, false); }

}  // namespace

// The issue's literal repro, with the equal boxed payload.
BEZEL_TEST(codegen_generic_record_equals_object_6748, issue_repro_equal_payload_true) {
    BEZEL_EXPECT_EQ(outU("var v := new V(1); var o: Object := new V(1); "
                        "Console.WriteLine(H.Eq<V>(v, o)); return 0; "),
                    std::string("True\n"));
}

BEZEL_TEST(codegen_generic_record_equals_object_6748, non_equal_and_non_t_arguments_false) {
    BEZEL_EXPECT_EQ(outU("var v := new V(1); var o2: Object := new V(2); "
                        "var w: Object := new W(1); var sb := new Animal(); "
                        "Console.WriteLine(H.Eq<V>(v, o2)); "
                        "Console.WriteLine(H.Eq<V>(v, w)); "
                        "Console.WriteLine(H.Eq<V>(v, sb)); return 0; "),
                    std::string("False\nFalse\nFalse\n"));
}

BEZEL_TEST(codegen_generic_record_equals_object_6748, not_equal_operator) {
    BEZEL_EXPECT_EQ(outU("var v := new V(1); var o: Object := new V(1); "
                        "var sb := new Animal(); "
                        "Console.WriteLine(H.Ne<V>(v, o)); "
                        "Console.WriteLine(H.Ne<V>(v, sb)); return 0; "),
                    std::string("False\nTrue\n"));
}

BEZEL_TEST(codegen_generic_record_equals_object_6748, object_on_the_left) {
    BEZEL_EXPECT_EQ(outU("var v := new V(1); var o: Object := new V(1); "
                        "var sb := new Animal(); "
                        "Console.WriteLine(H.EqR<V>(o, v)); "
                        "Console.WriteLine(H.EqR<V>(sb, v)); return 0; "),
                    std::string("True\nFalse\n"));
}

// a08_2: two type parameters.
BEZEL_TEST(codegen_generic_record_equals_object_6748, two_type_parameters) {
    BEZEL_EXPECT_EQ(outU("var v := new V(1); var o: Object := new V(1); "
                        "Console.WriteLine(H.Two<V, int>(v, 3, o)); "
                        "Console.WriteLine(H.Two<V, V>(v, v, new Animal())); return 0; "),
                    std::string("True\nFalse\n"));
}

// A record with a string field compares by content through the slot.
BEZEL_TEST(codegen_generic_record_equals_object_6748, string_field_record) {
    BEZEL_EXPECT_EQ(outU("var b := new B(1, \"x\"); var o: Object := new B(1, \"x\"); "
                        "var p: Object := new B(1, \"y\"); "
                        "Console.WriteLine(H.Eq<B>(b, o)); "
                        "Console.WriteLine(H.Eq<B>(b, p)); return 0; "),
                    std::string("True\nFalse\n"));
}

// A `T?` operand against `Object` (same §9.2 rule; a null `T?` is unequal to
// a non-null `Object`). The specialization resolved `T?` to the bare `T` and
// boxed the tagged `{i8, T}` carrier as a plain value (wrong answers, e.g.
// `A<V>(new V(2), vo)` True); the template pass crashed on base 45c70066.
BEZEL_TEST(codegen_generic_record_equals_object_6748, nullable_type_parameter_operand) {
    const std::string src =
        "module M; "
        "record V(X: int) end V; "
        "public class H "
        "  public static method A<T>(a: T?, b: Object): bool => a = b; "
        "  public static method N<T>(a: T?, b: Object): bool => b <> a; "
        "end H; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: Object := 5; var o2: Object := 6; var vo: Object := new V(1); "
        "    Console.WriteLine(H.A<int>(5, o)); Console.WriteLine(H.A<int>(5, o2)); "
        "    Console.WriteLine(H.A<int>(null, o)); Console.WriteLine(H.N<int>(5, o)); "
        "    Console.WriteLine(H.N<int>(null, o)); "
        "    Console.WriteLine(H.A<V>(new V(1), vo)); Console.WriteLine(H.A<V>(new V(2), vo)); "
        "    Console.WriteLine(H.A<V>(null, vo)); Console.WriteLine(H.N<V>(new V(1), vo)); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    // runAot: the record temporaries box and leak independently (#6769).
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True\nFalse\nFalse\nFalse\nTrue\n"
                                "True\nFalse\nFalse\nFalse\n"));
}

// Non-generic control: same answers. Its own head, with no generic method in
// it, so no template body is emitted for the program at all.
BEZEL_TEST(codegen_generic_record_equals_object_6748, non_generic_control) {
    const std::string src =
        "module M; "
        "record V(X: int) end V; "
        "record W(X: int) end W; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(1); var o: Object := new V(1); "
        "    var w: Object := new W(1); "
        "    Console.WriteLine(v = o); Console.WriteLine(v = w); "
        "    Console.WriteLine(v <> o); return 0; "
        "  end Main; "
        "end Program;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nFalse\n"));
}

// 200 iterations under the leak oracle (Animal argument: no record box); the
// count proves each call ran and answered unequal.
BEZEL_TEST(codegen_generic_record_equals_object_6748, loop_200_no_leak) {
    BEZEL_EXPECT_EQ(out("var v := new V(1); var n := 0; "
                        "for var i in 0..199 do "
                        "  if H.Eq<V>(v, new Animal()) then n := n + 1000; end; "
                        "  if H.Ne<V>(v, new Animal()) then n := n + 1; end; "
                        "end; Console.WriteLine(n); return 0; "),
                    std::string("200\n"));
}

// Same loop with an equal boxed payload; unchecked, see outU.
BEZEL_TEST(codegen_generic_record_equals_object_6748, loop_200_equal_payload) {
    BEZEL_EXPECT_EQ(outU("var v := new V(1); var n := 0; "
                         "for var i in 0..199 do "
                         "  if H.Eq<V>(v, new V(1)) then n := n + 1; end; "
                         "end; Console.WriteLine(n); return 0; "),
                    std::string("200\n"));
}

// ── Cross-module: the template body is what RUNS (#6748 fix-forward) ─────────
//
// An importing module binds `H.Eq<int>` (and the `Box<int>` vtable slot) to the
// defining module's template body, compiled against the `i32` placeholder;
// `int` is the only type argument the #1943 width gate lets cross. The first
// cut folded `T = Object` in that body to a constant, so these printed the
// opposite answer for an equal boxed payload. §9.2 (v0.340) "Through a type
// parameter" and §8.4: a boxed `int` payload that is equal compares True.

namespace {

const char* kXmodLib =
    "module Lib; "
    "public class H "
    "  public static method Eq<T>(a: T, b: Object): bool => a = b; "
    "  public static method Ne<T>(a: T, b: Object): bool => a <> b; "
    "  public static method OE<T>(a: Object, b: T): bool => a = b; "
    "  public static method Wrap<T>(a: T, b: Object): bool => Eq<T>(a, b); "
    "end H; "
    "public class Box<T> "
    "  private _v: T; "
    "  public constructor(v: T) _v := v; end constructor; "
    "  public method Same(o: Object): bool => _v = o; "
    "  public virtual method VSame(o: Object): bool => _v = o; "
    "  public virtual method VDiff(o: Object): bool => _v <> o; "
    "end Box; "
    "public class Outer<T> "
    "  public static method Go(a: T, o: Object): bool => H.Eq<T>(a, o); "
    "  public static method Go2(a: T, o: Object): bool => new Box<T>(a).Same(o); "
    "end Outer;";

std::string xmod(const std::string& body, bool checked = false) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", kXmodLib});
    mods.push_back({"App",
        "module App; import Lib; "
        "public class K end K; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: Object := 5; var o2: Object := 6; var s: Object := \"5\"; "
        + body +
        "    return 0; "
        "  end Main; "
        "end Program;"});
    JitResult r = checked
        ? bezel::test::runAotModulesChecked(std::move(mods), "App")
        : bezel::test::runAotModules(mods, "App");
    if (!r.ran || r.exitCode != 0)
        return "<<fail ran=" + std::to_string(r.ran) +
               " rc=" + std::to_string(r.exitCode) + ">>";
    return r.stdoutText;
}

}  // namespace

BEZEL_TEST(codegen_generic_record_equals_object_6748, xmod_static_eq_equal_and_unequal) {
    BEZEL_EXPECT_EQ(xmod("Console.WriteLine(H.Eq<int>(5, o)); "
                         "Console.WriteLine(H.Eq<int>(5, o2)); "
                         "Console.WriteLine(H.Eq<int>(5, s)); "),
                    std::string("True\nFalse\nFalse\n"));
}

BEZEL_TEST(codegen_generic_record_equals_object_6748, xmod_static_ne_equal_and_unequal) {
    BEZEL_EXPECT_EQ(xmod("Console.WriteLine(H.Ne<int>(5, o)); "
                         "Console.WriteLine(H.Ne<int>(5, o2)); "
                         "Console.WriteLine(H.Ne<int>(5, s)); "),
                    std::string("False\nTrue\nTrue\n"));
}

BEZEL_TEST(codegen_generic_record_equals_object_6748, xmod_static_object_on_the_left) {
    BEZEL_EXPECT_EQ(xmod("Console.WriteLine(H.OE<int>(o, 5)); "
                         "Console.WriteLine(H.OE<int>(o2, 5)); "
                         "Console.WriteLine(H.OE<int>(s, 5)); "),
                    std::string("True\nFalse\nFalse\n"));
}

// A template calling a template: both run the placeholder body.
BEZEL_TEST(codegen_generic_record_equals_object_6748, xmod_nested_template_calls) {
    BEZEL_EXPECT_EQ(xmod("Console.WriteLine(H.Wrap<int>(5, o)); "
                         "Console.WriteLine(H.Wrap<int>(5, o2)); "
                         "Console.WriteLine(Outer<int>.Go(5, o)); "
                         "Console.WriteLine(Outer<int>.Go(5, o2)); "
                         "Console.WriteLine(Outer<int>.Go2(5, o)); "
                         "Console.WriteLine(Outer<int>.Go2(5, o2)); "),
                    std::string("True\nFalse\nTrue\nFalse\nTrue\nFalse\n"));
}

// The virtual slot of an imported generic class holds the template.
BEZEL_TEST(codegen_generic_record_equals_object_6748, xmod_virtual_vsame_vdiff) {
    BEZEL_EXPECT_EQ(xmod("var b := new Box<int>(5); "
                         "Console.WriteLine(b.VSame(o)); "
                         "Console.WriteLine(b.VSame(o2)); "
                         "Console.WriteLine(b.VDiff(o)); "
                         "Console.WriteLine(b.VDiff(s)); "
                         "Console.WriteLine(b.Same(o)); "),
                    std::string("True\nFalse\nFalse\nTrue\nTrue\n"));
}

// 200 calls of each spelling with an equal boxed payload. runAotModules, not
// Checked: an `Object` local that is an `=` operand or a call argument leaks
// its own box once (pre-existing on 45c70066, #6149 / #6733); the row below
// carries the leak oracle.
BEZEL_TEST(codegen_generic_record_equals_object_6748, xmod_loop_200_equal_payload) {
    BEZEL_EXPECT_EQ(xmod("var b := new Box<int>(5); var n := 0; "
                         "for var i in 0..199 do "
                         "  if H.Eq<int>(5, o) then n := n + 1; end; "
                         "  if H.Ne<int>(5, o2) then n := n + 1000; end; "
                         "  if b.VSame(o) then n := n + 1000000; end; "
                         "end; Console.WriteLine(n); "),
                    std::string("200200200\n"));
}

// The same 600 template comparisons under the leak oracle, against a class
// argument (no box outside the template): the `int` box each comparison mints
// for the placeholder is released.
BEZEL_TEST(codegen_generic_record_equals_object_6748, xmod_loop_200_no_leak) {
    BEZEL_EXPECT_EQ(xmod("var b := new Box<int>(5); var n := 0; "
                         "for var i in 0..199 do "
                         "  if not H.Eq<int>(5, new K()) then n := n + 1; end; "
                         "  if H.Ne<int>(5, new K()) then n := n + 1000; end; "
                         "  if not b.VSame(new K()) then n := n + 1000000; end; "
                         "end; Console.WriteLine(n); ", /*checked=*/true),
                    std::string("200200200\n"));
}
