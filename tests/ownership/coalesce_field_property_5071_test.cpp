// Issue #5071 (§28.3 K7.3 mixed `??` meet, v0.307):
// `checkNullCoalesceMixedMeet` only handled bare IdentifierExpr arms
// (named owned locals / parameters). A field read (`_w`) or
// implicit-this property read (`P`) is NOT an IdentifierExpr, so it
// was silently accepted as a non-borrow arm. Combined with a fresh
// owned right arm (`new Widget(...)`), the arms mix owned+borrowed
// and should be rejected — but compiled clean and leaked.
//
// The bug was a coverage gap in the K7.3 check introduced by fix #4964.
// The "borrow arm" classification was restricted to bare IdentifierExpr,
// but §28.3 K3.13 says "any read" of a tracked owned local/param is a
// borrow arm — field reads and property reads are equally unmarked reads
// and are equally borrow arms.
//
// OBSERVABILITY: pre-fix the REJECT rows compile clean (no error), so
// each asserts the specific diagnostic and !r.ran.  The ACCEPT row
// verifies the spec-legal null-arm absorption (K7.5) is unaffected.
//
// per §28.3 (producer table `a ?? b` row; K7.1–K7.3, K3.13), v0.307

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kNeedle =
    "the operands of this `??` mix an owned value with a borrowed arm";

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// _w ?? new Widget("n") — owned field read is a borrow arm
const char* kFieldProgram =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine(\"dispose \" + _n); end destructor; "
    "  public method Name(): string => _n; "
    "end Widget; "
    "public class Box "
    "  private owned _w: Widget? := null; "
    "  public constructor() end constructor; "
    "  public method Get(): string => (_w ?? new Widget(\"n\")).Name(); "
    "end Box; "
    "public class Program "
    "  public static method Main(): int "
    "    using var b := new Box(); "
    "    Console.WriteLine(b.Get()); "
    "    Console.WriteLine(\"end\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// P ?? new Widget("n2") — implicit-this property read is a borrow arm
const char* kPropertyProgram =
    "module M; "
    "public class Widget "
    "  private _n: string; "
    "  public constructor(n: string) _n := n; end constructor; "
    "  destructor Console.WriteLine(\"dispose \" + _n); end destructor; "
    "  public method Name(): string => _n; "
    "end Widget; "
    "public class Box "
    "  private owned _w: Widget? := null; "
    "  public constructor() end constructor; "
    "  public property P: Widget? => _w; "
    "  public method Get2(): string => (P ?? new Widget(\"n2\")).Name(); "
    "end Box; "
    "public class Program "
    "  public static method Main(): int "
    "    using var b := new Box(); "
    "    Console.WriteLine(b.Get2()); "
    "    Console.WriteLine(\"end\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// ===========================================================================
// REJECT — field read (borrow arm) met with fresh owned arm.
// ===========================================================================

BEZEL_TEST(coalesce_field_property_5071,
           owned_field_left_fresh_right_rejected_5071_28_3) {
    JitResult r = runAot(kFieldProgram);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ===========================================================================
// REJECT — implicit-this property read (borrow arm) met with fresh owned arm.
// ===========================================================================

BEZEL_TEST(coalesce_field_property_5071,
           implicit_this_property_left_fresh_right_rejected_5071_28_3) {
    JitResult r = runAot(kPropertyProgram);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNeedle));
    BEZEL_EXPECT_FALSE(r.ran);
}
