// Codegen / refcounting — issue #5291 (sibling of #5290): a safe-navigation
// read as an interpolation hole, `{q?.Name}` (string-wide: `string` and
// `string?` alike), released a BORROWED reference: a heap-use-after-free.
// Fixed by the same hole rule as #5290 (see string_optional_hole_borrow_5290).
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

std::string nameClass(const std::string& ty) {
    return "public class D "
           "  public property Name: " + ty + "; "
           "  public constructor() " + init(ty, "Name") + " end constructor; "
           "  public method Get(): " + ty + " => Name; "
           "end D; ";
}

// Reads `V`/`Name` through a nullable-aware length: `x ?? ""` on `string?`,
// plain `.Length` on the `string` twin.
std::string len(const std::string& ty, const std::string& e) {
    return ty == "string?" ? "(" + e + " ?? \"\").Length" : e + ".Length";
}

}  // namespace

// #5291: `{q?.Name}` over a property. Pre-fix: heap-use-after-free for BOTH
// `string?` and `string`.
BEZEL_TEST(codegen_string_optional_hole_borrow_5291,
           safe_nav_field_hole_spec_18_5_28_9) {
    for (std::string ty : {"string?", "string"}) {
        BEZEL_EXPECT_EQ(runC(prog(nameClass(ty),
            "      using var d := new D(); "
            "      d.Name := \"w\" + i.ToString(); "
            "      var q: D? := d; "
            "      var s := $\"{q?.Name}-x\"; "
            "      t := t + s.Length + " + len(ty, "d.Name") + "; ")),
            sumOf([](int i) { return tag(i) + 2 + tag(i); }));
    }
}

// #5291: `{q?.Get()}` safe-navigation METHOD call. The callee returns a +1
// reference, so the hole must still release it (no leak, no double free).
BEZEL_TEST(codegen_string_optional_hole_borrow_5291,
           safe_nav_method_call_hole_spec_18_5_28_9) {
    for (std::string ty : {"string?", "string"}) {
        BEZEL_EXPECT_EQ(runC(prog(nameClass(ty),
            "      using var d := new D(); "
            "      d.Name := \"w\" + i.ToString(); "
            "      var q: D? := d; "
            "      var s := $\"{q?.Get()}-x\"; "
            "      t := t + s.Length + " + len(ty, "d.Name") + "; ")),
            sumOf([](int i) { return tag(i) + 2 + tag(i); }));
    }
}

// #5291: chain `a?.b?.c` (runL: the `move`d nullable-class field leaks with no
// hole at all, a separate defect).
BEZEL_TEST(codegen_string_optional_hole_borrow_5291,
           safe_nav_chain_hole_spec_18_5_28_9) {
    for (std::string ty : {"string?", "string"}) {
        BEZEL_EXPECT_EQ(runL(prog(
            "public class Leaf "
            "  public property C: " + ty + "; "
            "  public constructor() " + init(ty, "C") + " end constructor; "
            "end Leaf; "
            "public class Mid "
            "  public property B: Leaf?; "
            "  public constructor() end constructor; "
            "end Mid; ",
            "      var leaf := new Leaf(); "
            "      leaf.C := \"w\" + i.ToString(); "
            "      using var mid := new Mid(); "
            "      mid.B := move leaf; "
            "      var a: Mid? := mid; "
            "      var s := $\"{a?.B?.C}-x\"; "
            "      t := t + s.Length + (mid.B?.C ?? \"\").Length; ")),
            sumOf([](int i) { return tag(i) + 2 + tag(i); }));
    }
}

// `??` combined with `?.`: `{q?.Name ?? "d"}` and a null receiver.
BEZEL_TEST(codegen_string_optional_hole_borrow_5291,
           safe_nav_coalesce_hole_spec_18_5_28_9) {
    for (std::string ty : {"string?", "string"}) {
        BEZEL_EXPECT_EQ(runC(prog(nameClass(ty),
            "      using var d := new D(); "
            "      d.Name := \"w\" + i.ToString(); "
            "      var q: D? := d; "
            "      var n: D? := null; "
            "      var s := $\"{q?.Name ?? \"d\"}-{n?.Name ?? \"d\"}\"; "
            "      t := t + s.Length + " + len(ty, "d.Name") + "; ")),
            sumOf([](int i) { return tag(i) + 2 + tag(i); }));
    }
}

// `{n?.Name}` over a null receiver renders "null".
BEZEL_TEST(codegen_string_optional_hole_borrow_5291,
           safe_nav_null_receiver_hole_spec_18_5_28_9) {
    BEZEL_EXPECT_EQ(runC(prog(nameClass("string?"),
        "      var n: D? := null; "
        "      var s := $\"[{n?.Name}]\"; "
        "      t := t + s.Length; ")),
        sumOf([](int) { return 6; }));
}

// `?.` outside an interpolation: bind, argument, return (#5291 sibling check).
BEZEL_TEST(codegen_string_optional_hole_borrow_5291,
           safe_nav_outside_interpolation_spec_28_9) {
    for (std::string ty : {"string?", "string"}) {
        BEZEL_EXPECT_EQ(runC(prog(nameClass(ty) +
            "public class H "
            "  static method Take(p: string?): int => (p ?? \"\").Length; "
            "  public static method Via(q: D?): string? => q?.Name; "
            "  public static method Run(i: int): int "
            "    using var d := new D(); "
            "    d.Name := \"w\" + i.ToString(); "
            "    var q: D? := d; "
            "    var x := q?.Name; "
            "    var y := Via(q); "
            "    return (x ?? \"\").Length + Take(q?.Name) + (y ?? \"\").Length; "
            "  end Run; "
            "end H; ",
            "      t := t + H.Run(i); ")),
            sumOf([](int i) { return 3 * tag(i); }));
    }
}
