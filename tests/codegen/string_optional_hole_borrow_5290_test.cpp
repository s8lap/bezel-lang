// Codegen / refcounting — issues #5290 and #5291: a `string?` interpolation
// hole over a computed property, `Array<string?>[i]`, `Dictionary<K,string?>[k]`
// (#5290) and a safe-navigation read `{q?.Name}` (#5291, string-wide) released
// a BORROWED reference: a refcount over-release abort or a heap-use-after-free.
//
// Spec: §18.5 (string interpolation: a hole renders via `ToString`, a null
// `string?` renders "null"), §28.9 (runtime-managed types: released exactly
// once, never freed while reachable), the null-conditional operator `?.`
// (§7.5), spec revision v0.313.
//
// Root: #5277 whitelisted plain slot reads as borrows; every other `string?`
// hole kept the "`pv != srcEv` means a fresh render" rule and released the
// PHI(live, "null"). A `string?` hole renders its live arm by identity, so it
// is exactly the non-nullable `string` hole: a borrow unless the hole is a +1
// producer (isPlusOneStringProducer). `?.` yields a `string?` read of the
// property's storage, so it is the same case.
//
// Every row loops 200 times, builds each string at RUNTIME and compares the
// full stdout. Rows loop over a `string?` and a `string` twin where the shape
// allows. runAotCheckedLenient (LeakSanitizer off, generated-code ASan on) is
// used only where the program leaks WITHOUT any interpolation hole (an
// `Array<string?>` / `Dictionary<K,string?>` local, a `move`d nullable-class
// field, a lambda-captured `string?` local: separate defects); the
// use-after-free and over-release these rows pin are still caught.

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

// Expected stdout: the sum over the 200 iterations of f(i), then a newline.
template <class F>
std::string sumOf(F f) {
    int w = 0;
    for (int i = 0; i < kIters; ++i) w += f(i);
    return std::to_string(w) + "\n";
}

// Length of "<one-char tag><i>".
int tag(int i) { return 1 + digits(i); }

std::string prog(const std::string& helpers, const std::string& body) {
    return "module M; " + helpers +
           "public class Program "
           "  public static method Main(): int "
           "    var t := 0; "
           "    for var i in 0..<200 do " + body +
           "    end; "
           "    Console.WriteLine(t); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// A non-nullable `string` slot needs a constructor initialiser.
std::string init(const std::string& ty, const std::string& slot) {
    return ty == "string" ? slot + " := \"init\";" : "";
}

std::string computedClass(const std::string& ty) {
    return "public class K "
           "  private _f: " + ty + "; "
           "  public constructor() " + init(ty, "_f") + " end constructor; "
           "  public method Put(x: " + ty + ") _f := x; end Put; "
           "  public property V: " + ty + " => _f; "
           "end K; ";
}

// Reads `V`/`Name` through a nullable-aware length: `x ?? ""` on `string?`,
// plain `.Length` on the `string` twin.
std::string len(const std::string& ty, const std::string& e) {
    return ty == "string?" ? "(" + e + " ?? \"\").Length" : e + ".Length";
}

}  // namespace

// #5290: computed property. Pre-fix: exit 134 "refcount over-release (string)".
BEZEL_TEST(codegen_string_optional_hole_borrow_5290,
           computed_property_hole_spec_18_5_28_9) {
    for (std::string ty : {"string?", "string"}) {
        BEZEL_EXPECT_EQ(runC(prog(computedClass(ty),
            "      using var k := new K(); "
            "      k.Put(\"w\" + i.ToString()); "
            "      var s := $\"{k.V}-x\"; "
            "      k.Put(\"z\" + i.ToString()); "
            "      t := t + s.Length + " + len(ty, "k.V") + "; ")),
            sumOf([](int i) { return tag(i) + 2 + tag(i); }));
    }
}

// #5290: `Array<string?>[i]`. Pre-fix: heap-use-after-free.
BEZEL_TEST(codegen_string_optional_hole_borrow_5290,
           array_element_hole_spec_18_5_28_9) {
    for (std::string ty : {"string?", "string"}) {
        BEZEL_EXPECT_EQ(runL(prog("",
            "      var a := new Array<" + ty + ">(); "
            "      a.Add(\"w\" + i.ToString()); "
            "      var s := $\"{a[0]}-x\"; "
            "      t := t + s.Length + " + len(ty, "a[0]") + "; ")),
            sumOf([](int i) { return tag(i) + 2 + tag(i); }));
    }
}

// #5290: `Dictionary<int, string?>[k]`. Pre-fix: heap-use-after-free.
BEZEL_TEST(codegen_string_optional_hole_borrow_5290,
           dictionary_value_hole_spec_18_5_28_9) {
    for (std::string ty : {"string?", "string"}) {
        BEZEL_EXPECT_EQ(runL(prog("",
            "      var m := new Dictionary<int, " + ty + ">(); "
            "      m.Add(i, \"w\" + i.ToString()); "
            "      var s := $\"{m[i]}-x\"; "
            "      t := t + s.Length + " + len(ty, "m[i]") + "; ")),
            sumOf([](int i) { return tag(i) + 2 + tag(i); }));
    }
}

// A `string?`-returning call is a +1 producer: the hole still releases it.
BEZEL_TEST(codegen_string_optional_hole_borrow_5290,
           producer_call_hole_still_released_spec_18_5_28_9) {
    BEZEL_EXPECT_EQ(runC(prog(
        "public class P "
        "  public static method Mk(i: int): string? "
        "    if i mod 3 = 0 then return null; end; "
        "    return \"m\" + i.ToString(); "
        "  end Mk; "
        "end P; ",
        "      var s := $\"{P.Mk(i)}-x\"; "
        "      t := t + s.Length; ")),
        sumOf([](int i) { return (i % 3 == 0 ? 4 : tag(i)) + 2; }));
}

// A lambda-captured `string?` as a hole (runL: the captured `string?` local
// leaks 7290 B / 200 iterations with NO hole at all, a separate defect).
BEZEL_TEST(codegen_string_optional_hole_borrow_5290,
           lambda_captured_hole_spec_18_5_28_9) {
    BEZEL_EXPECT_EQ(runL(prog("",
        "      var p: string? := \"w\" + i.ToString(); "
        "      var f := lambda() => $\"{p}-x\"; "
        "      var s := f(); "
        "      t := t + s.Length; ")),
        sumOf([](int i) { return tag(i) + 2; }));
}

// Guard (passes before and after): a call through a `() -> string?` lambda and
// through an interface are +1 producers, so the hole still releases them.
BEZEL_TEST(codegen_string_optional_hole_borrow_5290,
           lambda_and_interface_call_hole_still_released_spec_18_5_28_9) {
    BEZEL_EXPECT_EQ(runC(prog(
        "public interface IG "
        "  method G(i: int): string?; "
        "end IG; "
        "public class Impl: IG "
        "  public constructor() end constructor; "
        "  public method G(i: int): string? => \"g\" + i.ToString(); "
        "end Impl; ",
        "      using var impl := new Impl(); "
        "      var ig: IG := impl; "
        "      var f := lambda(k: int): string? => \"l\" + k.ToString(); "
        "      var s := $\"{f(i)}-{ig.G(i)}\"; "
        "      t := t + s.Length; ")),
        sumOf([](int i) { return 2 * tag(i) + 1; }));
}
