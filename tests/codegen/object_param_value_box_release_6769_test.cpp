// Regression test for issue #6769.
//
// §8.6 (v0.344), "Boxing": a value passed to an `Object` parameter is boxed at
// the call; the box is runtime-managed (§28.9) and the callee borrows it. The
// caller releases the box after the call exactly when the callee provably does
// not let the parameter escape (fix #2952, `ParamEscapeWalker`). The walker
// counted `valueKind.Equals(param)` and `valueKind = param` as escapes, so a
// callee that only COMPARES the parameter leaked every argument box: 24 B per
// call, 200 x 24 = 4800 B in the issue's repro. A record / enum / choice /
// primitive receiver dispatches through the compiler-generated box-equals
// thunk, which only reads `other`, so the comparison is a read (§8.1, §8.3,
// §8.4). A class receiver is NOT such a read (a user `Equals` override may
// store its borrowed parameter, #3480) and stays unreleased.
//
// §25.2 (v0.344) draws the line the OTHER way for a comparison the value kind
// OVERLOADS: "the overloaded operators are invoked for `a = b` and `a <> b`
// directly ... they do not delegate to `Equals`". Such an overload is user
// code, its body may STORE the parameter's box, and the caller must not
// release it — the last two rows pin that.
//
// Every checked row loops 200x: a one-shot LSan row is no oracle (#4638).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kHead =
    "module M; "
    "record V(X: int) end V; "
    "enum Col Red, Green, Blue; end Col; "
    "choice Sh Circle(R: int), Dot; end Sh; "
    "public class Holder "
    "  public Kept: Object? := null; "
    "  public method Keep(o: Object): bool "
    "    this.Kept := o; return true; "
    "  end Keep; "
    "end Holder; "
    "public class Cls "
    "  public override method Equals(other: Object?): bool => true; "
    "  public override method GetHashCode(): int => 1; "
    "end Cls; "
    "public class H "
    "  public static method EqM(a: V, b: Object): bool => a.Equals(b); "
    "  public static method EqO(a: V, b: Object): bool => a = b; "
    "  public static method NeO(a: V, b: Object): bool => a <> b; "
    "  public static method EqRev(a: V, b: Object): bool => b = a; "
    "  public static method EqE(a: Col, b: Object): bool => a.Equals(b); "
    "  public static method EqS(a: Sh, b: Object): bool => a.Equals(b); "
    "  public static method EqI(a: int, b: Object): bool => a.Equals(b); "
    "  public static method EqIO(a: int, b: Object): bool => a = b; "
    "  public static method Show(b: Object): int "
    "    Console.WriteLine(b); return 1; "
    "  end Show; "
    "  public static method Pass(a: V, b: Object): Object "
    "    if a.Equals(b) then return b; end; return b; "
    "  end Pass; "
    "  public static method Store(a: V, h: Holder, b: Object): bool "
    "    if a.Equals(b) then return h.Keep(b); end; return false; "
    "  end Store; "
    "  public static method ViaClass(c: Cls, b: Object): bool => c.Equals(b); "
    "end H; ";

std::string runWith(const char* head, const std::string& body,
                    bool checked = true) {
    const std::string src = std::string(head) + "public class Program "
        "  public static method Main(): int " + body + " end Main; "
        "end Program;";
    JitResult r = checked ? runAotChecked(src) : runAot(src);
    if (!r.ran || r.exitCode != 0)
        return "<<fail rc=" + std::to_string(r.exitCode) + ">>";
    return r.stdoutText;
}

std::string run(const std::string& body, bool checked = true) {
    return runWith(kHead, body, checked);
}

// A value kind that OVERLOADS `=`/`<>`. §25.2 (v0.344): "the overloaded
// operators are invoked for `a = b` and `a <> b` directly ... they do not
// delegate to `Equals`" — the comparison is USER CODE, and its body may
// STORE the parameter's box (§28.3's field-store residual admits an `Object`
// parameter into a field). The caller must therefore NOT release the box at
// the call: the parameter escaped.
const char* kHeadOp =
    "module M; "
    "public class Holder "
    "  public Kept: Object? := null; "
    "end Holder; "
    "public static class Sink "
    "  public static readonly H: Holder := new Holder(); "
    "end Sink; "
    "public record V(X: int) "
    "  public static method operator =(a: V, b: Object): bool "
    "    Sink.H.Kept := b; return true; "
    "  end operator =; "
    "  public static method operator <>(a: V, b: Object): bool "
    "    Sink.H.Kept := b; return false; "
    "  end operator <>; "
    "end V; "
    "public enum Col "
    "  Red, Green, Blue; "
    "  public static method operator =(a: Col, b: Object): bool "
    "    Sink.H.Kept := b; return true; "
    "  end operator =; "
    "  public static method operator <>(a: Col, b: Object): bool "
    "    Sink.H.Kept := b; return false; "
    "  end operator <>; "
    "end Col; "
    "public class H "
    "  public static method EqV(a: V, b: Object): bool => a = b; "
    "  public static method EqC(a: Col, b: Object): bool => a = b; "
    "end H; ";

}  // namespace

// The issue's literal repro.
BEZEL_TEST(codegen_object_param_value_box_release_6769, issue_repro_record_temporary_equals) {
    BEZEL_EXPECT_EQ(run("var v := new V(1); var n := 0; "
                        "for var i in 0..199 do "
                        "  if H.EqM(v, new V(1)) then n := n + 1; end; end; "
                        "Console.WriteLine(n); return 0; "),
                    std::string("200\n"));
}

// Named local (boxed at the call) vs temporary, `=` spelling, `<>`, reversed.
BEZEL_TEST(codegen_object_param_value_box_release_6769, record_equals_operator_spellings) {
    BEZEL_EXPECT_EQ(run("var v := new V(1); var w := new V(2); var n := 0; "
                        "for var i in 0..199 do "
                        "  if H.EqO(v, v) then n := n + 1; end; "
                        "  if H.EqO(v, new V(1)) then n := n + 1; end; "
                        "  if H.NeO(v, new V(2)) then n := n + 1; end; "
                        "  if H.EqRev(v, w) then n := n + 100; end; "
                        "end; "
                        "Console.WriteLine(n); return 0; "),
                    std::string("600\n"));
}

// Enum, choice and int receivers.
BEZEL_TEST(codegen_object_param_value_box_release_6769, enum_choice_int_receivers) {
    BEZEL_EXPECT_EQ(run("var n := 0; var c := Col.Green; var s := Sh.Circle(3); "
                        "for var i in 0..199 do "
                        "  if H.EqE(c, Col.Green) then n := n + 1; end; "
                        "  if H.EqS(s, Sh.Circle(3)) then n := n + 1; end; "
                        "  if H.EqI(7, 7) then n := n + 1; end; "
                        "  if H.EqIO(7, 8) then n := n + 100; end; "
                        "end; "
                        "Console.WriteLine(n); return 0; "),
                    std::string("600\n"));
}

// Different payload / different type answer False, boxes still released.
BEZEL_TEST(codegen_object_param_value_box_release_6769, unequal_arguments_answer_false) {
    BEZEL_EXPECT_EQ(run("var v := new V(1); var n := 0; "
                        "for var i in 0..199 do "
                        "  if H.EqM(v, new V(2)) then n := n + 1; end; "
                        "  if H.EqM(v, 1) then n := n + 1; end; "
                        "  if H.EqM(v, \"x\") then n := n + 1; end; "
                        "end; "
                        "Console.WriteLine(n); return 0; "),
                    std::string("0\n"));
}

// `Console.WriteLine(Object)` callee: the boxed argument is rendered, no leak.
BEZEL_TEST(codegen_object_param_value_box_release_6769, console_writeline_object_callee) {
    std::string expected;
    for (int i = 0; i < 200; ++i) expected += std::to_string(i) + "\n";
    expected += "200\n";
    BEZEL_EXPECT_EQ(run("var n := 0; "
                        "for var i in 0..199 do n := n + H.Show(i); end; "
                        "Console.WriteLine(n); return 0; "),
                    expected);
}

// A callee that RETURNS the parameter: the box is the result, never released
// at the call, and stays readable.
BEZEL_TEST(codegen_object_param_value_box_release_6769, callee_returning_the_object) {
    BEZEL_EXPECT_EQ(run("var v := new V(1); var n := 0; "
                        "for var i in 0..199 do "
                        "  var r := H.Pass(v, new V(1)); "
                        "  if r is V then n := n + 1; end; "
                        "end; "
                        "Console.WriteLine(n); return 0; ", false),
                    std::string("200\n"));
}

// A callee that STORES the parameter into a field must not have it released
// (that would be a use-after-free): read the stored box after the call.
BEZEL_TEST(codegen_object_param_value_box_release_6769, callee_storing_the_object_no_uaf) {
    BEZEL_EXPECT_EQ(run("var v := new V(1); var h := new Holder(); var n := 0; "
                        "for var i in 0..199 do "
                        "  if H.Store(v, h, new V(1)) then "
                        "    var k := h.Kept; "
                        "    if k is V then n := n + 1; end; "
                        "  end; "
                        "end; "
                        "Console.WriteLine(n); return 0; ", false),
                    std::string("200\n"));
}

// A class receiver may run a user Equals override that stores its parameter
// (#3480): the box is left alone. Answer is correct and nothing is freed early.
BEZEL_TEST(codegen_object_param_value_box_release_6769, class_receiver_still_not_released) {
    BEZEL_EXPECT_EQ(run("var c := new Cls(); var n := 0; "
                        "for var i in 0..199 do "
                        "  if H.ViaClass(c, new V(1)) then n := n + 1; end; "
                        "end; "
                        "Console.WriteLine(n); return 0; ", false),
                    std::string("200\n"));
}

// A record that OVERLOADS `=`/`<>` (§25.2, v0.344): the comparison is the
// author's operator, not the generated box-equals thunk, and the operator
// STORES the parameter's box. The caller must leave that box alone.
//
// MEASURED on the arm WITHOUT this guard (the ported fix before it was
// hardened): the caller released the box at the call, and the stored
// reference then read back as NOT-`V` — and under ASan, reading it was a
// heap-use-after-free ("READ of size 8 ... freed by thread T0 here: ...
// bz_box_release (src/runtime/bezel_runtime_refcounting.cpp:705)"). With the
// guard the stored box is still the callee's, and the type test and the
// payload read both answer.
//
// These rows are `runAot`, NOT the checked oracle: the CORRECT answer leaves
// the box with the callee, and a box held by a field is runtime-managed
// (§28.9) — nobody frees it, so the checked oracle would report the leak
// that is the point of the row.
BEZEL_TEST(codegen_object_param_value_box_release_6769,
           user_equals_operator_keeps_the_box) {
    BEZEL_EXPECT_EQ(runWith(kHeadOp,
                        "var n := 0; "
                        "for var i in 0..2 do "
                        "  if H.EqV(new V(1), new V(1)) then n := n + 1; end; "
                        "end; "
                        "var k := Sink.H.Kept; "
                        "if k is V then n := n + 100 + k.X; end; "
                        "Console.WriteLine(n); return 0; ", false),
                    std::string("104\n"));
}

// The same shape on an enum: §25.2 admits an enum body what an enum body
// admits, operator overloads included (§10.5).
BEZEL_TEST(codegen_object_param_value_box_release_6769,
           user_equals_operator_enum_keeps_the_box) {
    BEZEL_EXPECT_EQ(runWith(kHeadOp,
                        "var n := 0; "
                        "for var i in 0..2 do "
                        "  if H.EqC(Col.Green, Col.Green) then n := n + 1; end; "
                        "end; "
                        "var k := Sink.H.Kept; "
                        "if k is Col then n := n + 100; end; "
                        "Console.WriteLine(n); return 0; ", false),
                    std::string("103\n"));
}
