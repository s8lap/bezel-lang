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

// Non-generic control: same answers.
BEZEL_TEST(codegen_generic_record_equals_object_6748, non_generic_control) {
    BEZEL_EXPECT_EQ(outU("var v := new V(1); var o: Object := new V(1); "
                        "var w: Object := new W(1); "
                        "Console.WriteLine(v = o); Console.WriteLine(v = w); "
                        "Console.WriteLine(v <> o); return 0; "),
                    std::string("True\nFalse\nFalse\n"));
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
