// Regression tests for #8142 (§8.5 / §18.5, v0.355): a CLASS-typed
// interpolation hole must render the RECEIVER's `ToString` via VIRTUAL
// dispatch — the same dispatch the member call `b.ToString()` uses.
//
// §8.5 (v0.355), first sentence, verbatim:
//   "`ToString` is invoked implicitly inside string interpolation (§18.5) via
//    virtual dispatch — for present values; a `null` value of a nullable-typed
//    hole renders as the text `null` with no dispatch (§18.5)."
// and §8.5's class bullet:
//   "For classes: the type's **qualified name** — the module-path-qualified
//    rendering stdlib §2.13 defines for `Type.FullName`".
//
// THE DEFECT. The interpolation-hole lowering RESOLVED `ToString` against the
// STATIC class chain and, finding none, answered with the STATIC type's
// qualified name. The member call `b.ToString()` dispatched VIRTUALLY (the
// receiver's own vtable, slot 3). Two spellings of one rule therefore
// disagreed — the issue's own repro printed:
//
//   M.Base       <- $"{b}"       (the hole: the STATIC type's name)
//   D!           <- b.ToString() (the member call: the runtime override)
//   M.Base2      <- $"{b2}"
//   M.Derived2   <- b2.ToString()
//
// The default must come from the RUNTIME class: `M.Derived2`, not `M.Base2`.
//
// THE FIX. The hole renders a class/interface operand through the ONE
// renderer (`emitValueToString`), which dispatches `ToString` through the
// RECEIVER's vtable — the runtime class's override if it declares one, else
// the RUNTIME class's qualified name. The hole's own second copy of the
// dispatch (the static-chain walk) is deleted, so the hole, the aligned hole
// (§18.5), the console's argument rendering and the record component all
// answer alike.
//
// ORACLE. Every row is on the checked oracle (`runAotChecked` — ASan/LSan,
// `detect_leaks=1`): a hole that renders through the vtable must not leak
// the render, double-free it, or crash. The rendered OUTPUT is asserted
// EXACTLY (the whole stdout), so a row cannot pass by rendering the wrong
// text — and the "unchanged" rows (the record hole, the choice hole) cannot
// pass by rendering something else either.
//
// The 100x row is the AMPLIFIED matrix: every shape above, one hundred
// times, under the sanitizer — the §28.3 / §28.9 leak/crash net at the
// issue's own amplification.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked — the checked oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's own types, plus the shapes the brief enumerates.
//
//   Base   — a class with NO `ToString` at all. A `Base`-typed hole holding
//            a `Derived` must render the DERIVED class's answer.
//   Derived— overrides `ToString` ("D!").
//   Base2  — a class with NO `ToString`; `Derived2` has none either, so the
//            §8.5 default applies and it must be the RUNTIME class's
//            qualified name ("M.Derived2"), not the static one's
//            ("M.Base2").
//   IShape — an interface (a control: the interface arm already dispatched
//            virtually, #1077).
//   Sealed — a `sealed` class (a control: nothing derives from it, so the
//            static and the runtime class are the same).
//   Holder — a class-typed FIELD, to cover the field shape.
//   Rec    — a record (a control: the record hole's own rendering).
//   R2     — a record with a CLASS-typed component: §8.5's component rule
//            says "Each component renders as its own `ToString` renders it
//            (virtual dispatch — a class-typed component with no override
//            renders its class default, the qualified name per the bullet
//            above)".
//   Color  — a choice (a control: the choice hole's own rendering).
const char* kPrelude = R"(
module M;
public class Base
  public N: int := 4242;
end Base;
public class Derived: Base
  public override method ToString(): string => "D!";
end Derived;
public class Base2
  public N: int := 4242;
end Base2;
public class Derived2: Base2
  public M2: int := 1;
end Derived2;
public interface IShape end IShape;
public class Impl: IShape
  public override method ToString(): string => "I!";
end Impl;
public sealed class Sealed
  public override method ToString(): string => "S!";
end Sealed;
public class Holder
  public B: Base;
  public constructor(b: Base) B := b; end constructor;
end Holder;
public record Rec(X: int);
public record R2(B: Base);
public choice Color Red, Green(value: int), Blue; end Color;
)";

// The program's helpers: a call RESULT (`owned Base`), a PARAMETER, and a
// generic method whose type parameter is instantiated at the BASE class.
const char* kHelpers = R"(
public class Program
  public static method Make(): owned Base => new Derived();
  public static method Show(b: Base): string => $"{b}";
  public static method ShowT<T>(x: T): string => $"{x}";
)";

} // namespace

// ── the issue's own repro, verbatim ──────────────────────────────────────────
//
// MEASURED on the pre-fix compiler (the issue's numbers, reproduced):
//   M.Base / D! / M.Base2 / M.Derived2
// and after the fix:
//   D! / D! / M.Derived2 / M.Derived2
//
// Each line is a hole or the explicit call on the SAME value, so the row
// fails if the two ever disagree again.
BEZEL_TEST(interp_hole_virtual_tostring_8142,
           issue_repro_hole_and_member_call_agree) {
    auto r = runAotChecked(R"(
module M;
public class Base
  public N: int := 4242;
end Base;
public class Derived: Base
  public override method ToString(): string => "D!";
end Derived;
public class Base2
  public N: int := 4242;
end Base2;
public class Derived2: Base2
  public M2: int := 1;
end Derived2;
public class Program
  public static method Main(): int
    var b: Base := new Derived();
    Console.WriteLine($"{b}");
    Console.WriteLine(b.ToString());
    var b2: Base2 := new Derived2();
    Console.WriteLine($"{b2}");
    Console.WriteLine(b2.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "D!\nD!\nM.Derived2\nM.Derived2\n");
}

// ── the SHAPES a class-typed hole reaches ────────────────────────────────────
//
// One row per operand shape the brief enumerates: a base-typed LOCAL, a
// FIELD, a PARAMETER, and a CALL RESULT. Each line pairs the hole with the
// explicit member call where there is one, so the row is a comparison, not
// just a literal.
BEZEL_TEST(interp_hole_virtual_tostring_8142,
           local_field_parameter_and_call_result_dispatch_virtually) {
    auto r = runAotChecked(std::string(kPrelude) + kHelpers + R"(
  public static method Main(): int
    var b: Base := new Derived();
    var h: Holder := new Holder(b);
    Console.WriteLine($"local|{b}|{b.ToString()}");
    Console.WriteLine($"field|{h.B}|{h.B.ToString()}");
    Console.WriteLine($"param|{Show(b)}");
    Console.WriteLine($"call|{Make()}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     "local|D!|D!\n"
                     "field|D!|D!\n"
                     "param|D!\n"
                     "call|D!\n");
}

// ── the CONTROLS: interface, generic T at a class, sealed class ─────────────
//
// These three already dispatched virtually (the interface arm, #1077; the
// generic substitution, #1118; the sealed class's own override) — they are
// here so the fix cannot quietly move them.
BEZEL_TEST(interp_hole_virtual_tostring_8142,
           interface_generic_and_sealed_controls_unchanged) {
    auto r = runAotChecked(std::string(kPrelude) + kHelpers + R"(
  public static method Main(): int
    var i: IShape := new Impl();
    var b: Base := new Derived();
    var d: Derived := new Derived();
    var s: Sealed := new Sealed();
    Console.WriteLine($"iface|{i}|{i.ToString()}");
    Console.WriteLine($"genericBase|{ShowT<Base>(b)}");
    Console.WriteLine($"genericDerived|{ShowT<Derived>(d)}");
    Console.WriteLine($"sealed|{s}|{s.ToString()}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     "iface|I!|I!\n"
                     "genericBase|D!\n"
                     "genericDerived|D!\n"
                     "sealed|S!|S!\n");
}

// ── the RECORD and CHOICE holes: UNCHANGED ──────────────────────────────────
//
// A record hole renders the record's own form and a choice hole the choice's
// own case-shaped form (§8.5's record / choice bullets) — neither is a
// class-operand hole, and neither moves.
//
// The last line is a record with a CLASS-typed component: §8.5's component
// rule renders it by the component's own `ToString`, i.e. VIRTUALLY, so the
// component renders the RUNTIME class's answer ("D!" — the `Derived`
// instance's override) rather than the static type's name ("M.Base") — the
// same rule the hole above applies, at the component level.
BEZEL_TEST(interp_hole_virtual_tostring_8142,
           record_and_choice_holes_unchanged) {
    auto r = runAotChecked(std::string(kPrelude) + kHelpers + R"(
  public static method Main(): int
    var b: Base := new Derived();
    var r2: R2 := new R2(b);
    Console.WriteLine($"record|{new Rec(1)}");
    Console.WriteLine($"recordClass|{r2}");
    Console.WriteLine($"choice|{Color.Green(7)}");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     "record|Rec(X: 1)\n"
                     "recordClass|R2(B: D!)\n"
                     "choice|Color.Green(value: 7)\n");
}

// ── the ALIGNED hole (§18.5) and the CONSOLE's argument ─────────────────────
//
// `$"{b,10}"` applies §18.5's alignment to the RENDERED text, so it must
// render the same text the plain hole renders, padded. And the console
// renders "its one argument" through the same `ToString`, so it must answer
// the same.
BEZEL_TEST(interp_hole_virtual_tostring_8142,
           aligned_hole_and_console_argument_render_the_same_text) {
    auto r = runAotChecked(std::string(kPrelude) + kHelpers + R"(
  public static method Main(): int
    var b: Base := new Derived();
    Console.WriteLine($"aligned|{b,10}|");
    Console.WriteLine(b);
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     "aligned|        D!|\n"
                     "D!\n");
}

// ── the AMPLIFIED matrix: every shape, 100x, under ASan/LSan ────────────────
//
// The leak/crash net at the issue's own amplification: 100 evaluations of
// every shape, each rendering through the vtable. A render that leaked (the
// fresh +1 the vtable thunk mints), double-freed (the receiver's own
// reference) or crashed (a wild vtable jump) would flip `r.ran` to false.
//
// The stdout is asserted EXACTLY — 100 copies of the matrix's lines — so a
// row cannot pass by rendering the wrong text either.
BEZEL_TEST(interp_hole_virtual_tostring_8142,
           matrix_100x_under_asan_no_leak_or_crash_flip) {
    auto r = runAotChecked(std::string(kPrelude) + kHelpers + R"(
  public static method Main(): int
    var i := 0;
    while i < 100 do
      var b: Base := new Derived();
      var b2: Base2 := new Derived2();
      var h: Holder := new Holder(b);
      var i2: IShape := new Impl();
      var d: Derived := new Derived();
      var r2: R2 := new R2(b);
      var sc: Sealed := new Sealed();
      Console.WriteLine($"local|{b}|{b.ToString()}");
      Console.WriteLine($"local2|{b2}|{b2.ToString()}");
      Console.WriteLine($"field|{h.B}|{h.B.ToString()}");
      Console.WriteLine($"param|{Show(b)}");
      Console.WriteLine($"call|{Make()}");
      Console.WriteLine($"iface|{i2}|{i2.ToString()}");
      Console.WriteLine($"genericBase|{ShowT<Base>(b)}");
      Console.WriteLine($"genericDerived|{ShowT<Derived>(d)}");
      Console.WriteLine($"sealed|{sc}|{sc.ToString()}");
      Console.WriteLine($"record|{new Rec(1)}");
      Console.WriteLine($"recordClass|{r2}");
      Console.WriteLine($"choice|{Color.Green(7)}");
      Console.WriteLine($"aligned|{b,10}|");
      Console.Write(b);
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string one =
        "local|D!|D!\n"
        "local2|M.Derived2|M.Derived2\n"
        "field|D!|D!\n"
        "param|D!\n"
        "call|D!\n"
        "iface|I!|I!\n"
        "genericBase|D!\n"
        "genericDerived|D!\n"
        "sealed|S!|S!\n"
        "record|Rec(X: 1)\n"
        "recordClass|R2(B: D!)\n"
        "choice|Color.Green(value: 7)\n"
        "aligned|        D!|\n"
        // `Console.Write` appends NO line terminator, so the block's last
        // line runs into the next iteration's first one.
        "D!";
    std::string expected;
    expected.reserve(one.size() * 100);
    for (int k = 0; k < 100; ++k) expected += one;
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}
