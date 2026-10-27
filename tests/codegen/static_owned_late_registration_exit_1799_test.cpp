// Codegen / runtime — the program-exit disposal pass for static `owned` fields
// DRAINS late-registered entries (spec §22.13, v0.196; issue #1799).
//
// §22.13 (v0.196): "Static `owned` fields initialized during the pass join it:
// the initialization appends to the recorded order, and the pass drains until no
// initialized-but-undisposed static `owned` field remains, taking late-
// initialized entries in reverse order of their own initialization; strict
// reverse-initialization order is guaranteed only among initializations completed
// before shutdown began (a mid-pass initialization cannot precede disposals
// already performed)."
//
// THE BUG (#1799). A destructor invoked during the exit pass may make the FIRST
// touch of an as-yet-uninitialized type, whose lazy static initialization
// (permitted at exit under §22.12 rule 1) constructs a static owned field and
// calls bz_register_static_owned — appending to the exit registry MID-PASS. The
// old bz_dispose_static_owned iterated a fixed index range whose upper bound was
// the registry size captured once at entry (`for i = size()-1 .. 0`), so such a
// late entry was never revisited: it was left UNDISPOSED (its destructor never
// ran) and its heap block leaked. The fix drains the registry LIFO
// (pop-from-back) until empty, so a mid-pass push_back is taken next.
//
// OBSERVABILITY. The leaked late object stays *reachable* from its type's global
// static slot, so default LeakSanitizer (as runAotChecked configures it) does
// NOT flag it — a still-reachable leak (this is inherent to every static-owned-
// field leak, #1212 included). The reliable, spec-aligned observable is therefore
// whether the late field's DESTRUCTOR RUNS: each destructor prints a marker during
// the exit pass, captured in the subprocess's stdout. Without the drain fix the
// late marker never appears; with it, it appears exactly once. runAotChecked is
// used so the with-fix path is additionally guarded against a double-free / use-
// after-free the drain could introduce (dispose-exactly-once net).

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// #1799: Early.res (EarlyRes) is the ONLY static owned field initialized before
// shutdown — Main touches type Early (reads Early.tag), which lazily initializes
// its statics and registers Early.res. Type Late is NOT touched during Main.
// At exit, EarlyRes's destructor makes the first touch of Late (writes Late.tag),
// triggering Late's lazy static init -> new LateRes() -> registered MID-PASS.
// §22.13 requires the pass to drain until LateRes is disposed too.
const char* kLateRegistrationDuringExit =
    "module M; "
    "class LateRes "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"LATE-DISPOSED\"); end destructor; "
    "end LateRes; "
    "class Late "
    "  public static owned readonly res: LateRes := new LateRes(); "
    "  public static tag: int := 1; "               // mutable static: a write forces Late's init
    "end Late; "
    "class EarlyRes "
    "  public constructor() end constructor; "
    "  destructor "
    "    Console.WriteLine(\"EARLY-DISPOSED\"); "
    "    Late.tag := Late.tag + 1; "                 // first touch of Late -> late registration
    "  end destructor; "
    "end EarlyRes; "
    "class Early "
    "  public static owned readonly res: EarlyRes := new EarlyRes(); "
    "  public static tag: int := 7; "
    "end Early; "
    "class Prog "
    "  public static method Main(): int => Early.tag; "  // triggers Early init only (not Late)
    "end Prog;";

// Regression: ordinary static owned fields (both initialized before shutdown) are
// still disposed exactly once, in reverse declaration order (§22.13). r1 then r2
// initialize when C is first touched; disposal is r2 (ResB) then r1 (ResA).
const char* kOrdinaryReverseOrder =
    "module M; "
    "class ResA "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DISPOSE-A\"); end destructor; "
    "end ResA; "
    "class ResB "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DISPOSE-B\"); end destructor; "
    "end ResB; "
    "class C "
    "  public static owned readonly r1: ResA := new ResA(); "
    "  public static owned readonly r2: ResB := new ResB(); "
    "  public static tag: int := 5; "
    "end C; "
    "class Prog "
    "  public static method Main(): int => C.tag; "
    "end Prog;";

}  // namespace

// =====================================================================
// #1799 — the late-registered static owned field is disposed by the drain.
// =====================================================================

BEZEL_TEST(codegen_static_owned_late_registration_1799,
           late_field_disposed_during_exit_drain_spec_22_13) {
    // spec §22.13, v0.196: a static owned field whose lazy initialization is first
    // triggered by an earlier destructor during the exit pass appends to the
    // recorded order, and the pass drains until it is disposed. Without the fix the
    // late field is stranded undisposed — "LATE-DISPOSED" never appears.
    JitResult r = runAotChecked(kLateRegistrationDuringExit);

    // Clean run (Main returns Early.tag = 7); the drain introduced no double-free /
    // use-after-free (ASan/LSan net rides on r.ran).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);

    // Load-bearing #1799 assertion: the LATE-registered field WAS disposed, exactly
    // once. Absent the drain fix this count is 0 (the fixed-bound loop never
    // revisits the mid-pass entry).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "LATE-DISPOSED"), 1);

    // The early field disposed first; the late field, appended mid-pass, disposed
    // after it (§22.13: "a mid-pass initialization cannot precede disposals already
    // performed").
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "EARLY-DISPOSED"), 1);
    const std::size_t early = r.stdoutText.find("EARLY-DISPOSED");
    const std::size_t late = r.stdoutText.find("LATE-DISPOSED");
    BEZEL_EXPECT_TRUE(early != std::string::npos);
    BEZEL_EXPECT_TRUE(late != std::string::npos);
    BEZEL_EXPECT_TRUE(early < late);
}

// =====================================================================
// Regression — ordinary static owned fields still disposed exactly once,
// in reverse declaration order.
// =====================================================================

BEZEL_TEST(codegen_static_owned_late_registration_1799,
           ordinary_static_owned_reverse_order_still_holds_spec_22_13) {
    // §22.13: within a type, multiple static owned fields dispose in reverse
    // declaration order. The LIFO drain must preserve this for pre-exit entries:
    // r2 (ResB) disposes before r1 (ResA).
    JitResult r = runAotChecked(kOrdinaryReverseOrder);

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DISPOSE-A"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DISPOSE-B"), 1);
    const std::size_t a = r.stdoutText.find("DISPOSE-A");
    const std::size_t b = r.stdoutText.find("DISPOSE-B");
    BEZEL_EXPECT_TRUE(a != std::string::npos);
    BEZEL_EXPECT_TRUE(b != std::string::npos);
    BEZEL_EXPECT_TRUE(b < a);  // reverse declaration order: r2 (B) before r1 (A)
}
