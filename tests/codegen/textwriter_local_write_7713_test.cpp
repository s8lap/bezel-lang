// Spec-derived regression test for issue #7713 — `WriteLine` on a
// `TextWriter`-typed receiver failed with "no dispatch arm emitted any
// code", so the call wrote nothing.
//
// THE SPEC.
//
//   stdlib §7.4 (v0.350) declares the `TextWriter` Write / WriteLine /
//   Flush members, and stdlib §2.1 (v0.350) makes `Console.Out` and
//   `Console.Error` instances of §2.1's rule - so a writer bound to one of
//   them writes what `Console.Out.<m>` / `Console.Error.<m>` write.
//
//   spec §8.6 (v0.350) owns the BOXING of a value at an `Object` position:
//   "reaching a location of type `Object` boxes the value, and boxing
//   copies it into a heap wrapper, which move-only types forbid".  An
//   owning-classified choice is a move-only value type (spec §15.3), so it
//   cannot be boxed - and spec §35.8 (v0.350) says an owning choice is not
//   boxable at all.
//
// THE DEFECT.
//
// A `TextWriter`-typed receiver had NO lowering at all: the call reported
// "no dispatch arm emitted any code" for EVERY argument - an `int`, a
// `string`, an `Object`, a class - and the program printed nothing.  The
// reviewer's owning-choice probe was one instance of the same shape.
//
// THE ORACLE IS POSITIVE.
//
//   * the two value-like rows assert the TEXT the writer actually wrote;
//   * the owning-choice row asserts the refusal and the POSITIVE diagnostic
//     text that §8.6 requires, at a REAL source location.
//
// No row asserts that a diagnostic is absent.
//
// The `Console.Out` VALUE has no runtime representation of its own - it
// lowers to null - so the STREAM is read off the declaring initializer.  The
// internal #2434 note the initializer emits is that lowering, and it is not
// an error: the row's oracle is the written text.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ===========================================================================
// THE VALUE-LIKE CONTROL — a `TextWriter` local bound to `Console.Out`
// writes what `Console.Out.WriteLine` writes.
// ===========================================================================
BEZEL_TEST(textwriter_local_write_7713,
           textwriter_local_writeline_writes_the_text) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var w: TextWriter := Console.Out; "
        "    w.WriteLine(\"hello\"); ")),
        std::string("hello"));
}

// ===========================================================================
// THE `Write` OVERLOAD — the same local, the other member of stdlib §7.4's
// surface.
// ===========================================================================
BEZEL_TEST(textwriter_local_write_7713,
           textwriter_local_write_writes_the_text) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var w: TextWriter := Console.Out; "
        "    w.Write(\"abc\"); ")),
        std::string("abc"));
}

// ===========================================================================
// THE REVIEWER'S OWN PROBE — an OWNING-CHOICE argument.
//
// The `TextWriter`-typed receiver is the same `Object` boxing position as
// `Console`'s, so the owning choice cannot be boxed (spec §8.6 / §35.8): it
// must be REFUSED, at a real location, naming the rule.
// ===========================================================================
BEZEL_TEST(textwriter_local_write_7713,
           textwriter_local_writeline_refuses_an_owning_choice_argument) {
    auto r = runAot(
        "module M; "
        "class Res public constructor() end constructor; end Res; "
        "choice OC2 Text(owned r: Res), Empty; end OC2; "
        "class Bx public owned V: OC2 := OC2.Empty; end Bx; "
        "class C "
        "  public static method Main(): int "
        "    var bx := new Bx(); "
        "    var w: TextWriter := Console.Out; "
        "    w.WriteLine(bx.V); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "owning-classified choice"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§8.6"));
}
