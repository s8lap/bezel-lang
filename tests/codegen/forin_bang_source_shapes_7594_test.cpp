// Codegen/ownership -- the remaining `!` spellings of a for-in SOURCE (issue
// #7594, a follow-up to #7526; spec v0.350).
//
// #7526 taught classifyBuiltinForInSource and lowerIEnumerableBuiltinFor to look
// through a postfix `!`. Five more classifications of the same source still
// keyed on the bare AST shape and so misread the `!` spelling:
//   S1  `for var x in (if c then h.Mk() else null)! do` -- an if-expression
//       producer (array protocol chosen: ZERO iterations, frame leaked).
//   S2  `for var x in f.E! do` over an INTERFACE property -- the #2779
//       interface-dispatched widening (isInterfaceDispatchedIteratorRead) did
//       not fire, so the minted wrapper frame leaked.
//   S3  `var e := f.E!; for var x in e do` -- the bound spelling of S2
//       (registerIterableSourceDisposal).
//   S4  `var e := h.Mk()!;` / `using var e := h.Mk()!;` iterated afterwards --
//       the bound producer was never claimed (registerIterableSourceDisposal /
//       registerUsingDisposal).
//   S6  `var a: Array<int>? := [1, 2, 3]; for var x in a! do` -- the array
//       local's borrow-read verdict (the escape scan) did not look through `!`,
//       so the array was treated as escaped and never reclaimed.
//
// Spec, v0.350:
//   sec 17.8: "if `x` has ownership state owned ... `x!` is owned; if `x` is
//   borrowed, `x!` is borrowed with the same source." and "Over a named owned
//   binding, `x!` is therefore classified by its position exactly as the bare
//   read `x` is."
//   sec 29.2: a `for` over a fresh class-typed expression (a call, a `new`)
//   assumes the disposal obligation; over a named owned local it only borrows.
//   sec 29.7: the loop's own enumerator is disposed on every exit.
//
// Every row loops 50x under generated-code ASan/LSan and pins the exact stdout;
// each `!` row has a no-bang control that must stay green.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// stdout (trailing newline stripped) of a clean, ASan/LSan-checked run, or a
// marker naming the failure (compile error, non-zero exit incl. a leak report).
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + ">";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// A holder whose `Mk()` yields a fresh `owned IEnumerable<int>?` frame and
// whose generator's `finally` counts how many times it ran.
const char* kHolder =
    "public class H\n"
    "  public static fins: int := 0;\n"
    "  public constructor() end constructor;\n"
    "  public method GenS(): owned IEnumerable<int>\n"
    "    try yield return 1; yield return 2;\n"
    "    finally H.fins := H.fins + 1; end;\n"
    "  end GenS;\n"
    "  public method Mk(): owned IEnumerable<int>?\n"
    "    return this.GenS();\n"
    "  end Mk;\n"
    "end H;\n";

std::string holderProg(const std::string& loopBody) {
    return std::string("module M;\n") + kHolder +
           "public class C\n"
           "  public static method Main(): int\n"
           "    var sum := 0; var i := 0;\n"
           "    while i < 50 do\n"
           "      using var h := new H();\n"
           "      var c := i > -1;\n" +
           loopBody +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(\"sum=\" + sum.ToString() + \" fins=\" + "
           "H.fins.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// An interface `ISeq` exposing `E: IEnumerable<Item>?`, implemented by a holder
// over an `OwningArray<Item>` (three items 3, 4, 5 => 12 per pass).
std::string ifaceProg(const std::string& loopBody) {
    return std::string(
        "module M;\n"
        "interface ISeq\n"
        "  readonly property E: IEnumerable<Item>?;\n"
        "end ISeq;\n"
        "class Item\n"
        "  public V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "end Item;\n"
        "class Holder : ISeq\n"
        "  private owned _a: OwningArray<Item>;\n"
        "  public constructor()\n"
        "    _a := new OwningArray<Item>();\n"
        "    _a.Add(new Item(3)); _a.Add(new Item(4)); _a.Add(new Item(5));\n"
        "  end constructor;\n"
        "  public property E: IEnumerable<Item>?\n"
        "    get => do\n"
        "      return _a;\n"
        "    end;\n"
        "  end E;\n"
        "end Holder;\n"
        "class P\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..50 do\n"
        "      using var h := new Holder();\n"
        "      var f: ISeq := h;\n") +
        loopBody +
        "    end;\n"
        "    Console.WriteLine(\"total=\" + total.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
}

std::string arrayProg(const std::string& loopBody) {
    return std::string(
               "module M;\n"
               "public class C\n"
               "  public static method Main(): int\n"
               "    var sum := 0; var i := 0;\n"
               "    while i < 50 do\n"
               "      var a: Array<int>? := [1, 2, 3];\n") +
           loopBody +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(\"sum=\" + sum.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// --- S1: an if-expression producer under `!` ---------------------------------

BEZEL_TEST(codegen_forin_bang_shapes_7594, s1_if_expression_producer_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(holderProg("      for var x in (if c then h.Mk() else null)! do "
                       "sum := sum + x; end;\n")),
        "sum=150 fins=50");
}

// Control: the same if-expression with two fresh non-null producers, no `!`.
BEZEL_TEST(codegen_forin_bang_shapes_7594, s1_control_if_expression_no_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(holderProg("      for var x in (if c then h.GenS() else h.GenS()) "
                       "do sum := sum + x; end;\n")),
        "sum=150 fins=50");
}

// --- S2: an interface property read under `!` --------------------------------

BEZEL_TEST(codegen_forin_bang_shapes_7594, s2_interface_property_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(ifaceProg("      for var x in f.E! do total := total + x.V; end;\n")),
        "total=600");
}

BEZEL_TEST(codegen_forin_bang_shapes_7594, s2_control_interface_property_no_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(ifaceProg("      for var x in f.E do total := total + x.V; end;\n")),
        "total=600");
}

// --- S3: the bound interface property read -----------------------------------

BEZEL_TEST(codegen_forin_bang_shapes_7594, s3_bound_interface_property_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(ifaceProg("      var e := f.E!;\n"
                      "      for var x in e do total := total + x.V; end;\n")),
        "total=600");
}

BEZEL_TEST(codegen_forin_bang_shapes_7594, s3_control_bound_interface_property_no_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(ifaceProg("      var e := f.E;\n"
                      "      for var x in e do total := total + x.V; end;\n")),
        "total=600");
}

// --- S4: a bound fresh producer under `!` ------------------------------------

BEZEL_TEST(codegen_forin_bang_shapes_7594, s4_var_bound_producer_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(holderProg("      var e := h.Mk()!;\n"
                       "      for var x in e do sum := sum + x; end;\n")),
        "sum=150 fins=50");
}

BEZEL_TEST(codegen_forin_bang_shapes_7594, s4_control_var_bound_producer_no_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(holderProg("      var e := h.Mk();\n"
                       "      for var x in e do sum := sum + x; end;\n")),
        "sum=150 fins=50");
}

BEZEL_TEST(codegen_forin_bang_shapes_7594, s4_using_var_bound_producer_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(holderProg("      using var e := h.Mk()!;\n"
                       "      for var x in e do sum := sum + x; end;\n")),
        "sum=150 fins=50");
}

BEZEL_TEST(codegen_forin_bang_shapes_7594, s4_control_using_var_bound_producer_no_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(holderProg("      using var e := h.Mk();\n"
                       "      for var x in e do sum := sum + x; end;\n")),
        "sum=150 fins=50");
}

// --- S6: a nullable Array local under `!` ------------------------------------

BEZEL_TEST(codegen_forin_bang_shapes_7594, s6_nullable_array_local_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(arrayProg("      for var x in a! do sum := sum + x; end;\n")),
        "sum=300");
}

BEZEL_TEST(codegen_forin_bang_shapes_7594, s6_control_nullable_array_local_no_bang_50x) {
    BEZEL_EXPECT_EQ(
        out(arrayProg("      for var x in a do sum := sum + x; end;\n")),
        "sum=300");
}
