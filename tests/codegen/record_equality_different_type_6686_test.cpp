// Regression test for issue #6686 (owner ruling on #6210, DECIDED v0.338).
//
// §8.4 (v0.339): "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper".
// §25.2 (v0.339): "The one fallback is equality: `=` and `<>` with no
// applicable overload are §8.4's equality, which governs them."
// §9.2 (v0.339): "Through the slot ... the member runs only on a `T` payload
// and any other argument compares unequal without the body running."
//
// THE DEFECT. `=` / `<>` between a record and a class value (or an int), or on
// a record whose only `operator =` is inapplicable to the right operand,
// crashed bezelc (SIGSEGV, rc 139) in Emitter::emitStructEquality: the
// struct-equality arm keyed on the LEFT operand's LLVM type and compared it
// against a right operand of another LLVM type. Ruling: the answer is `False`
// (`True` for `<>`), the operands are still evaluated, and an owned temporary
// operand is still disposed.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kHead =
    "module M; "
    "public interface IA end IA; "
    "public class Animal: IA end Animal; "
    "record V(X: int) end V; "
    "record P(X: int) end P; "
    "record Q(X: int) end Q; "
    "record G<T>(X: T) end G; "
    "record VI(X: int): IA end VI; "
    "record W(X: int) "
    "  public static method operator =(v: W, p: IA): bool => true; "
    "  public static method operator <>(v: W, p: IA): bool => false; "
    "end W; "
    "public class Tick "
    "  public static method Say(): int Console.WriteLine(\"tick\"); return 5; end Say; "
    "  public static method Make(): owned Animal => new Animal(); "
    "end Tick; ";

std::string prog(const std::string& body) {
    return std::string(kHead) +
           "public class Program "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end Program;";
}

std::string out(const std::string& body) {
    JitResult r = runAotChecked(prog(body));
    if (!r.ran || r.exitCode != 0) return "<<fail rc=" + std::to_string(r.exitCode) + ">>";
    return r.stdoutText;
}

std::string outUnchecked(const std::string& body) {
    JitResult r = runAot(prog(body));
    if (!r.ran || r.exitCode != 0) return "<<fail rc=" + std::to_string(r.exitCode) + ">>";
    return r.stdoutText;
}

}  // namespace

// The issue's literal repro: record vs class.
BEZEL_TEST(codegen_record_equality_different_type_6686, record_vs_class_false) {
    BEZEL_EXPECT_EQ(out("var v := new V(1); var s := new StringBuilder(); "
                        "Console.WriteLine(v = s); Console.WriteLine(v <> s); return 0; "),
                    std::string("False\nTrue\n"));
}

BEZEL_TEST(codegen_record_equality_different_type_6686, record_vs_int_false) {
    BEZEL_EXPECT_EQ(out("var v := new V(1); "
                        "Console.WriteLine(v = 1); Console.WriteLine(v <> 1); "
                        "Console.WriteLine(1 = v); return 0; "),
                    std::string("False\nTrue\nFalse\n"));
}

// §25.2: a record whose only `operator =` is inapplicable falls back to §8.4.
BEZEL_TEST(codegen_record_equality_different_type_6686, inapplicable_operator_eq_vs_stringbuilder_and_int) {
    BEZEL_EXPECT_EQ(out("var w := new W(1); var s := new StringBuilder(); "
                        "Console.WriteLine(w = s); Console.WriteLine(w <> s); "
                        "Console.WriteLine(w = 3); Console.WriteLine(w <> 3); "
                        "var a := new Animal(); Console.WriteLine(w = a); return 0; "),
                    std::string("False\nTrue\nFalse\nTrue\nTrue\n"));
}

// An operand with a side effect still runs exactly once per spelling.
BEZEL_TEST(codegen_record_equality_different_type_6686, operand_side_effect_runs) {
    BEZEL_EXPECT_EQ(out("var v := new V(1); "
                        "Console.WriteLine(v = Tick.Say()); Console.WriteLine(v <> Tick.Say()); return 0; "),
                    std::string("tick\nFalse\ntick\nTrue\n"));
}

// An owned temporary operand is disposed (ASan/LSan oracle: runAotChecked).
BEZEL_TEST(codegen_record_equality_different_type_6686, owned_temporary_disposed) {
    BEZEL_EXPECT_EQ(out("var v := new V(1); "
                        "Console.WriteLine(v = new Animal()); "
                        "Console.WriteLine(v <> Tick.Make()); "
                        "Console.WriteLine(new Animal() = v); return 0; "),
                    std::string("False\nTrue\nFalse\n"));
}

// 200 iterations: a leak would show under LSan; the count proves the fold ran.
BEZEL_TEST(codegen_record_equality_different_type_6686, loop_200_no_leak) {
    BEZEL_EXPECT_EQ(out("var v := new V(1); var n := 0; "
                        "for var i in 0..199 do "
                        "  if v = Tick.Make() then n := n + 1000; end; "
                        "  if v <> new Animal() then n := n + 1; end; "
                        "end; Console.WriteLine(n); return 0; "),
                    std::string("200\n"));
}

// Controls: a same-type pair and an applicable operator are unchanged.
BEZEL_TEST(codegen_record_equality_different_type_6686, same_type_and_applicable_unchanged) {
    BEZEL_EXPECT_EQ(out("var a := new V(1); var b := new V(1); var c := new V(2); "
                        "var w := new W(1); var an := new Animal(); "
                        "Console.WriteLine(a = b); Console.WriteLine(a = c); "
                        "Console.WriteLine(w = an); Console.WriteLine(w <> an); return 0; "),
                    std::string("True\nFalse\nTrue\nFalse\n"));
}

// #6210 owner ruling, record-pair case: two distinct records with the same field
// layout are different value types, so `=` answers False and `<>` True.
BEZEL_TEST(codegen_record_equality_different_type_6686, distinct_records_same_layout_false) {
    BEZEL_EXPECT_EQ(out("var p := new P(1); var q := new Q(1); "
                        "Console.WriteLine(p = q); Console.WriteLine(p <> q); return 0; "),
                    std::string("False\nTrue\n"));
}

// Same generic record at different instantiations: distinct value types.
BEZEL_TEST(codegen_record_equality_different_type_6686, generic_record_different_instantiation_false) {
    BEZEL_EXPECT_EQ(out("var a := new G<int>(1); var b := new G<long>(1L); "
                        "Console.WriteLine(a = b); Console.WriteLine(a <> b); return 0; "),
                    std::string("False\nTrue\n"));
}

// Accept guards: a record against an interface / Object local holding an EQUAL
// record still answers True (the fold must not over-fire).
BEZEL_TEST(codegen_record_equality_different_type_6686, record_vs_interface_local_equal_true) {
    BEZEL_EXPECT_EQ(out("var v := new VI(1); var i: IA := v; "
                        "Console.WriteLine(v = i); Console.WriteLine(i = v); "
                        "Console.WriteLine(v <> i); return 0; "),
                    std::string("True\nTrue\nFalse\n"));
}

// Uses runAot, not runAotChecked: `record = Object-local` leaks the operand box
// independently of this fold (#6733); this row pins the printed answers only.
BEZEL_TEST(codegen_record_equality_different_type_6686, record_vs_object_local_equal_true) {
    BEZEL_EXPECT_EQ(outUnchecked("var v := new V(1); var o: Object := v; var p: Object := new V(2); "
                        "Console.WriteLine(v = o); Console.WriteLine(o = v); "
                        "Console.WriteLine(v <> o); Console.WriteLine(v = p); return 0; "),
                    std::string("True\nTrue\nFalse\nFalse\n"));
}
