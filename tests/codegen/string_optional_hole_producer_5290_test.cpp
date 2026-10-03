// Codegen / refcounting — fix-forward for #5290 / #5291 (and #5302): a
// `string?` value is a +1 producer exactly when its `string` twin is.
//
// Spec: §18.5 (string interpolation: a hole renders via `ToString`, a null
// `string?` renders "null"), §28.3 (producers; the `if`/`case` arm-meet),
// §28.9 (runtime-managed types: released exactly once, never freed while
// reachable), §7.5 (`?.`), §7.7 (`??`), spec revision v0.313.
//
// The #5290 hole rule (borrow unless isPlusOneStringProducer) is right, but
// the predicate was stale for `string?`: an `if`/`case` whose arms the
// lowering normalizes to +1, a `??` over `string?`, a computed `string?`
// property with a producer body, and its `?.` read were all classified as
// borrows, so the hole leaked the +1 (and, before the hole rule, every other
// consumer did: bind, argument, `+`, `??`, `.Length`, `?.Length`, return).
// The fix makes the predicate agree with what each lowering returns for
// both `string` and `string?` (lowerCoalesce now normalizes a `string?` result
// the way it already did a `string` one).
//
// Every row loops 200 times, builds each string at RUNTIME, checks the full
// stdout, and runs a `string?` program and its `string` twin. Most rows
// consume the value TWICE, through the interpolation hole and through a
// `var` bind, so they pin both consumers of the shared predicate.
// runAotCheckedLenient (LeakSanitizer off, generated-code ASan on) is used only
// where a separate, pre-existing leak forces it; the row comment names the
// issue. A use-after-free / over-release is still caught there.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

constexpr int kIters = 200;

std::string runC(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
std::string runL(const std::string& src) {
    auto r = runAotCheckedLenient(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

int digits(int i) { return static_cast<int>(std::to_string(i).size()); }

template <class F>
std::string sumOf(F f) {
    int w = 0;
    for (int i = 0; i < kIters; ++i) w += f(i);
    return std::to_string(w) + "\n";
}

// Length of "<one-char tag><i>".
int tag(int i) { return 1 + digits(i); }
// Length of the computed property `W` / `WN`: "w" + len("w<i>").ToString().
int wlen(int i) { return 1 + digits(tag(i)); }

// `ty` selects the indexer's element type (the `string?` program or its
// `string` twin); everything else is declared in both forms.
std::string helpers(const std::string& ty) {
    return "public class K "
           "  private _f: string?; "
           "  private _g: string; "
           "  public constructor() _g := \"init\"; end constructor; "
           "  public method Put(x: string?) _f := x; _g := x ?? \"\"; end Put; "
           "  public property V: string? => _f; "
           "  public property VN: string => _g; "
           "  public property W: string? => \"w\" + (_f ?? \"\").Length.ToString(); "
           "  public property WN: string => \"w\" + (_f ?? \"\").Length.ToString(); "
           "  public property [j: int]: " + ty + " => " +
           (ty == "string" ? "_g" : "_f") + "; "
           "end K; "
           "public class Wrap "
           "  public property Name: string?; "
           "  public property Inner: K?; "
           "  public constructor() end constructor; "
           "end Wrap; "
           "public class P "
           "  public static method Fresh(i: int): string? => \"f\" + i.ToString(); "
           "  public static method FreshN(i: int): string => \"f\" + i.ToString(); "
           "end P; ";
}

std::string prog(const std::string& ty, const std::string& body) {
    return "module M; " + helpers(ty) +
           "public class Program "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<200 do "
           "      using var k := new K(); "
           "      k.Put(\"w\" + i.ToString()); "
           "      var q: K? := k; "
           "      var p: string? := \"p\" + i.ToString(); "
           "      var pn: string := \"p\" + i.ToString(); "
           "      var p0: string? := null; "
           "      if i mod 2 = 1 then p0 := \"p\" + i.ToString(); end; "
           "      var o: Object := \"o\" + i.ToString(); " + body +
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// Hole + bind over `e` (`string?`) or its twin `en` (`string`): t += len(s) +
// len(b), so the expected per-iteration total is 2 * len(value) + 2.
// `bindTy` is the twin's bind type (a `?.` read is `string?` in both).
std::string holeAndBind(const std::string& ty, const std::string& e,
                        const std::string& bindTy) {
    std::string b = bindTy == "string?" ? "(b ?? \"\").Length" : "b.Length";
    return "      var s := $\"{" + e + "}-x\"; "
           "      var b: " + bindTy + " := " + e + "; "
           "      t := t + s.Length + " + b + "; ";
}

void twin(const std::string& en, const std::string& es, int (*len)(int),
          bool twinIsNullable = false) {
    BEZEL_EXPECT_EQ(runC(prog("string?", holeAndBind("string?", en, "string?"))),
                    sumOf([&](int i) { return 2 * len(i) + 2; }));
    BEZEL_EXPECT_EQ(
        runC(prog("string", holeAndBind("string", es,
                                        twinIsNullable ? "string?" : "string"))),
        sumOf([&](int i) { return 2 * len(i) + 2; }));
}

}  // namespace

// --- arm-meet producers (§28.3): lowerConditional / lowerCaseExpr retain
// every non-producer arm, so the result carries +1 for `string?` too. -----

// Pre-fix: the hole leaked (LSan) on the picked #5290 commit, the bind leaked
// on r23-c28.
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           mixed_if_fresh_then_borrow_spec_28_3_18_5) {
    twin("(if i mod 2 = 0 then P.Fresh(i) else k.V)",
         "(if i mod 2 = 0 then P.FreshN(i) else k.VN)", tag);
}

BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           mixed_if_borrow_then_fresh_spec_28_3_18_5) {
    twin("(if i mod 2 = 0 then p else P.Fresh(i))",
         "(if i mod 2 = 0 then pn else P.FreshN(i))", tag);
}

// #5302: both arms borrowed (a local and a computed slot property).
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           if_both_arms_borrowed_5302_spec_28_3_18_5) {
    twin("(if i mod 2 = 0 then p else k.V)",
         "(if i mod 2 = 0 then pn else k.VN)", tag);
}

BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           case_fresh_and_borrow_arms_spec_28_3_18_5) {
    twin("(case i mod 2 of 0 => P.Fresh(i), _ => p)",
         "(case i mod 2 of 0 => P.FreshN(i), _ => pn)", tag);
}

// --- `??` (§7.7): lowerCoalesce normalizes each arm to +1. ----------------

BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           coalesce_fresh_lhs_spec_7_7_18_5) {
    twin("(P.Fresh(i) ?? p)", "(P.Fresh(i) ?? pn)", tag);
}

// p0 is null on even iterations, so both arms run.
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           coalesce_fresh_rhs_spec_7_7_18_5) {
    twin("(p0 ?? P.Fresh(i))", "(p0 ?? P.FreshN(i))", tag);
}

// Previously fixed abort shape (#5277 family): `??` over two borrows.
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           coalesce_borrow_borrow_spec_7_7_18_5) {
    twin("(p0 ?? k.V)", "(p0 ?? k.VN)", tag);
}

// --- computed properties: the inlined getter body decides. ---------------

BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           computed_property_fresh_body_spec_18_5_28_9) {
    twin("k.W", "k.WN", wlen);
}

// Previously fixed abort shape (#5290): a slot-body property is a borrow.
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           computed_property_borrow_body_spec_18_5_28_9) {
    twin("k.V", "k.VN", tag);
}

// `?.` over a fresh-body property (§7.5): the result is the body's +1.
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           nullcond_computed_property_fresh_body_spec_7_5_18_5) {
    twin("q?.W", "q?.WN", wlen, /*twinIsNullable=*/true);
}

// Previously fixed abort shape (#5291): `?.` over a slot-body property.
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           nullcond_computed_property_borrow_body_spec_7_5_18_5) {
    twin("q?.V", "q?.VN", tag, /*twinIsNullable=*/true);
}

// --- previously fixed abort shapes (#5277 / #5290 / #5291), pinned with
// their `string` twin. ---------------------------------------------------

BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           cast_as_string_spec_18_5_28_9) {
    twin("(o as string)", "((o as string) ?? \"\")", tag);
}

BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           indexer_slot_body_spec_18_5_28_9) {
    // `ty` picks the indexer type: `string?` reads _f, `string` reads _g.
    twin("k[0]", "k[0]", tag);
}

// `?.` chain through a null `Inner`: the hole renders "null", the bind is null.
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           nullcond_chain_null_link_spec_7_5_18_5) {
    for (std::string ty : {"string?", "string"}) {
        std::string member = ty == "string?" ? "V" : "VN";
        BEZEL_EXPECT_EQ(runC(prog(ty,
            "      using var w := new Wrap(); "
            "      w.Name := \"n\" + i.ToString(); "
            "      var qw: Wrap? := w; "
            "      var s := $\"{qw?.Name}-{qw?.Inner?." + member + "}\"; "
            "      var b: string? := qw?.Inner?." + member + "; "
            "      t := t + s.Length + (b ?? \"\").Length; ")),
            sumOf([](int i) { return tag(i) + 1 + 4; }));
    }
}

// --- the `?.Length` receiver (§7.5): the `?.` twin of the `.Length` #429
// release discharges a +1 producer receiver. ------------------------------

BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           nullcond_length_of_producer_receiver_spec_7_5_28_9) {
    BEZEL_EXPECT_EQ(runC(prog("string?",
        "      t := t + ((if i mod 2 = 0 then P.Fresh(i) else k.V)?.Length ?? 0) "
        "             + ((p0 ?? k.V)?.Length ?? 0) "
        "             + (k.W?.Length ?? 0); ")),
        sumOf([](int i) { return tag(i) + tag(i) + wlen(i); }));
    BEZEL_EXPECT_EQ(runC(prog("string",
        "      t := t + (if i mod 2 = 0 then P.FreshN(i) else k.VN).Length "
        "             + (p0 ?? k.VN).Length "
        "             + k.WN.Length; ")),
        sumOf([](int i) { return tag(i) + tag(i) + wlen(i); }));
}

// --- `return r;` of a producer-initialized local: an escaping local hands
// its +1 over (returnedStringLocalOwnsPlusOne) for `string?` as for `string`.
// Pre-fix the return minted a second reference: `var r: string? := "x" + …;
// return r;` leaked on r23-c28, and the fix-forward's wider producer set made
// `var r := n ?? p; return r;` (the #5253 guard row's shape) leak too. -----

BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           return_producer_initialized_local_spec_28_3_28_9) {
    for (std::string ty : {"string?", "string"}) {
        std::string len = ty == "string?" ? "(x ?? \"\").Length" : "x.Length";
        BEZEL_EXPECT_EQ(runC(
            "module M; "
            "public class Program "
            "  static method Len(x: " + ty + "): int => " + len + "; "
            "  static method Fresh(i: int): " + ty + " => \"f\" + i.ToString(); "
            "  static method Concat(i: int): " + ty + " "
            "    var r: " + ty + " := \"x\" + i.ToString(); "
            "    return r; "
            "  end Concat; "
            "  static method Call(i: int): " + ty + " "
            "    var r := Program.Fresh(i); "
            "    return r; "
            "  end Call; "
            "  static method Pick(n: string?, i: int): " + ty + " "
            "    var p: " + ty + " := \"p\" + i.ToString(); "
            "    var r := n ?? p; "
            "    return r; "
            "  end Pick; "
            "  public static method Main(): int "
            "    var t := 0; "
            "    for var i in 0..<200 do "
            "      t := t + Program.Len(Program.Concat(i)) "
            "             + Program.Len(Program.Call(i)) "
            "             + Program.Len(Program.Pick(null, i)) "
            "             + Program.Len(Program.Pick(\"q\" + i.ToString(), i)); "
            "    end; "
            "    Console.WriteLine(t); "
            "    return 0; "
            "  end Main; "
            "end Program;"),
            sumOf([](int i) { return 4 * tag(i); }));
    }
}

// --- Lenient rows: pre-existing leaks unrelated to the hole. -------------

// #5311: async string / string? returns are not normalized to +1, so every
// `await <call>` consumer leaks a producer body's result (the `string` twin
// leaks on r23-c28 too). Lenient keeps the over-release / UAF check: the
// pre-#5290 `string?` hole released this value, which aborts for a
// borrow-bodied async method (#5311's table).
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           await_hole_5311_spec_18_5_30_5) {
    for (std::string ty : {"string?", "string"}) {
        std::string src =
            "module M; "
            "public class K "
            "  private _f: " + ty + "; "
            "  public constructor() _f := \"init\"; end constructor; "
            "  public method Put(x: " + ty + ") _f := x; end Put; "
            "  public property V: " + ty + " => _f; "
            "end K; "
            "public class Program "
            "  static async method Fresh(i: int): Task<" + ty + "> "
            "    return \"a\" + i.ToString(); "
            "  end Fresh; "
            "  static async method Brw(k: K): Task<" + ty + "> "
            "    return k.V; "
            "  end Brw; "
            "  static async method H(i: int, k: K): Task<int> "
            "    var s := $\"{await Program.Fresh(i)}-{await Program.Brw(k)}\"; "
            "    return s.Length; "
            "  end H; "
            "  public static method Main(): int "
            "    var t := 0; "
            "    for var i in 0..<200 do "
            "      using var k := new K(); "
            "      k.Put(\"w\" + i.ToString()); "
            "      t := t + Program.H(i, k).Result; "
            "    end; "
            "    Console.WriteLine(t); "
            "    return 0; "
            "  end Main; "
            "end Program;";
        BEZEL_EXPECT_EQ(runL(src),
                        sumOf([](int i) { return 2 * tag(i) + 1; }));
    }
}

// #5300: a generic identity Mk<T>(x: T): T at T = string / string? leaks a
// borrowed argument (the `string` twin too). The fresh-argument form
// Mk<string?>(P.Fresh(i)) is a heap-use-after-free on r23-c28 and here
// (#5301), so it cannot be a row yet.
BEZEL_TEST(codegen_string_optional_hole_producer_5290,
           generic_identity_borrowed_arg_5300_spec_18_5_14_1) {
    for (std::string ty : {"string?", "string"}) {
        std::string arg = ty == "string?" ? "p" : "pn";
        std::string b = ty == "string?" ? "(b ?? \"\").Length" : "b.Length";
        BEZEL_EXPECT_EQ(runL(
            "module M; "
            "public class Program "
            "  static method Mk<T>(x: T): T => x; "
            "  public static method Main(): int "
            "    var t := 0; "
            "    for var i in 0..<200 do "
            "      var p: string? := \"p\" + i.ToString(); "
            "      var pn: string := \"p\" + i.ToString(); "
            "      var s := $\"{Mk<" + ty + ">(" + arg + ")}-x\"; "
            "      var b: " + ty + " := Mk<" + ty + ">(" + arg + "); "
            "      t := t + s.Length + " + b + "; "
            "    end; "
            "    Console.WriteLine(t); "
            "    return 0; "
            "  end Main; "
            "end Program;"),
            sumOf([](int i) { return 2 * tag(i) + 2; }));
    }
}
