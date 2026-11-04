// Disposal-correctness — an owned call result used as an interpolation hole
// (issue #8143; spec §18.5, §28.3.11, §28.3 K8.1 — all v0.355).
//
// §28.3.11 (v0.355), the rule the issue cites: an unbound owned temporary
// "used only by *borrowing* operations within the expression" is "disposed at
// the **end of the full expression** in which it is produced, after every use
// of it within that expression has completed".
//
// §18.5's hole dispatch only BORROWS the operand: the hole renders the value
// (dispatching the value's `ToString`, §18.5's format dispatch) and the
// rendered bytes are copied into the concatenation. So the +1 an
// `owned`-returning call transferred to the hole expression is owned by
// NOBODY — the expression ends and the object leaks. MEASURED on the issue's
// repro (`$"{C.Make()}"` in a 100-iteration loop): "SUMMARY: AddressSanitizer:
// 1600 byte(s) leaked in 100 allocation(s)", 16 B per evaluation.
//
// Each row is on the checked oracle (`runAotChecked` — ASan/LSan): a missed
// boundary disposal is a DIRECT leak (the LSan report flips `r.ran` to false)
// and a reclaim that wrongly claimed a BORROWED operand would be a
// double-free / use-after-free (also flipping `r.ran` to false). The rows
// that assert a rendered OUTPUT run on the plain AOT oracle (`runAot`) —
// §18.5's rendering is not a memory property.
//
// The rows are 100x loops (the issue's amplification) so a leak is 100
// allocations — LSan reports the total, and the per-evaluation destructor
// marker count pins "disposed exactly once per evaluation" from the other
// side.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked / runAot — AOT oracles
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// `C` — a plain destructorless-in-spirit class with a per-instance marker, so
// the destructor's OUTPUT counts the disposals. It declares NO `ToString`, so
// §8.5's class default renders it as its qualified name ("M.C") — the shape
// the issue's repro used. `Make` is the STATIC FACTORY; `Fresh` is the
// INSTANCE METHOD, both returning `owned C`.
//
// `F` — the §18.5 format-dispatch shape: a user type that opts into the
// auto-imported `IFormattable` and declares `ToString(format: string)`, so a
// hole with a format spec is admitted (§18.5's format dispatch) instead of
// rejected by the type-formattability check.
const char* kPrelude = R"(
module M;
public class C
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor Console.WriteLine("D" + Id.ToString()); end destructor;
  public static method Make(): owned C
    return new C(1);
  end Make;
  public method Fresh(): owned C
    return new C(2);
  end Fresh;
end C;
public class F: IFormattable
  public constructor() end constructor;
  destructor Console.WriteLine("DF"); end destructor;
  public override method ToString(): string => "F";
  public method ToString(format: string): string => $"fmt:{format}";
  public static method Make(): owned F
    return new F();
  end Make;
end F;
)";

} // namespace

// §28.3.11 / K8.1 (v0.355) — the STATIC FACTORY row: `$"{C.Make()}"`.
//
// The hole borrows the fresh `owned C` only to render it; the +1 must be
// discharged at the full expression's end. Without the discharge the row
// leaks 100 x 16 B (LSan fires → r.ran is false) and the destructor's marker
// never appears (0 x "D1").
BEZEL_TEST(interp_hole_owned_temp_8143,
           static_factory_hole_disposed_at_full_expression_boundary) {
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var i := 0;
    while i < 100 do
      Console.WriteLine($"{C.Make()}");
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 100 renders (§8.5's class default — the qualified name) and 100
    // disposals (the destructor's marker) — exactly one per evaluation.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "M.C"), 100);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 100);
}

// §28.3.11 / K8.1 (v0.355) — the INSTANCE METHOD row: `$"{c.Fresh()}"`.
//
// Same obligation, a different producer: the callee transfers +1 to the
// CALLER, and the caller (the hole expression) owns it alone. The receiver `c`
// is a BORROW of the local — the hole must not touch it.
BEZEL_TEST(interp_hole_owned_temp_8143,
           instance_method_hole_disposed_at_full_expression_boundary) {
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var c := new C(0);
    var i := 0;
    while i < 100 do
      Console.WriteLine($"{c.Fresh()}");
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "M.C"), 100);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 100);
    // The local `c` is the scope's own: disposed ONCE, at scope end.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D0"), 1);
}

// §18.5's format dispatch + §28.3.11 / K8.1 (v0.355) — the FORMAT SPEC row:
// `$"{F.Make():X}"`.
//
// The hole takes the format-spec path (§18.5: "A hole with a format spec
// dispatches to `ToString(format: string)`"), which is a DIFFERENT rendering
// path than the plain `ToString` dispatch — and the same ownership obligation.
BEZEL_TEST(interp_hole_owned_temp_8143,
           format_spec_hole_disposed_at_full_expression_boundary) {
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var i := 0;
    while i < 100 do
      Console.WriteLine($"{F.Make():X}");
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "fmt:X"), 100);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DF"), 100);
}

// §28.3.11 / K8.1 (v0.355) — the TWO HOLES row: `$"{C.Make()}-{C.Make()}"`.
//
// Two independent producers in ONE interpolation: each owns its own +1, each
// is discharged at the same boundary. 200 disposals for 200 evaluations (one
// hole would discharge only one of the two → 100 disposals and a 100 x 16 B
// leak).
BEZEL_TEST(interp_hole_owned_temp_8143,
           two_holes_each_disposed_at_full_expression_boundary) {
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var i := 0;
    while i < 100 do
      Console.WriteLine($"{C.Make()}-{C.Make()}");
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "M.C-M.C"), 100);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 200);
}

// CONTROL — the BORROWED VALUE hole: `$"{c}"` where `c` is a local.
//
// A borrowed operand is NOT an unbound owned temporary: the local owns it, and
// the hole only borrows it for the render. A reclaim that claimed it would
// free a value this scope does not own (a use-after-free at the scope-end
// disposal, and an early destructor). The destructor must run ONCE, and AFTER
// the render.
BEZEL_TEST(interp_hole_owned_temp_8143, borrowed_hole_is_not_disposed_by_the_hole) {
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var c := new C(7);
    Console.WriteLine($"{c}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    const std::string& out = r.stdoutText;
    BEZEL_EXPECT_EQ(countOccurrences(out, "M.C"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(out, "D7"), 1);
    const std::size_t rendered = out.find("M.C");
    const std::size_t disposed = out.find("D7");
    BEZEL_EXPECT_TRUE(rendered != std::string::npos &&
                      disposed != std::string::npos);
    BEZEL_EXPECT_TRUE(rendered < disposed);  // the scope end disposes it
}

// CONTROL — the STRING-LITERAL hole: `$"lit"` and a hole whose expression is a
// string literal.
//
// A literal is an immortal sentinel (a release on it is a no-op) and a
// string-typed hole is a BORROW (§28.9) — so neither may be reclaimed by the
// hole. This row runs on the plain AOT oracle (the rendering is not a memory
// property) and asserts the exact rendered text.
BEZEL_TEST(interp_hole_owned_temp_8143, string_literal_hole_renders_without_disposal) {
    auto r = runAot(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    Console.WriteLine($"lit");
    Console.WriteLine($"{"lit"}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "lit"), 2);
    // No destructor marker: nothing was produced to dispose.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D"), 0);
}
