// fix #8162 (stdlib §3.3, v0.355): `new ReadOnlyStringSpan(heapString, a, b)`
// LEAKED the source string — about 2 allocations per construction (the
// `bz_string` header + its payload), which is the issue's own measurement:
// "LeakSanitizer reports 4334 B in 202 allocations per 100 iterations".
//
// THE DEFECT. stdlib §3.3 (v0.355) defines the span's own factory in terms
// of this constructor — "`Slice` is `new ReadOnlyStringSpan(Source,
// this.Start + start, length)`" — and the span it builds is a
// {ptr,i32,i32} VALUE struct that BORROWS the source string's storage: it
// mints no reference of its own, exactly like the `AsSpan`/`Slice` factories
// the issue calls the clean twin. So the constructor's `Source` argument is a
// CALL-BOUNDED BORROW, and the SOURCE keeps its own reference: the source
// local's scope-end `bz_string_release` is what discharges it.
//
// The compiler implemented neither half of that. `new
// ReadOnlyStringSpan(w, 1, 3)` built the span struct and stored `w`'s raw
// pointer in it (correct — a borrow), but the conservative escape walk
// (`StringEscapeWalker::walkNewArgs`) classified EVERY argument of a `new
// T(...)` expression as an ESCAPE, because a constructor's argument was
// assumed to be stored into a heap object that outlives the scope. The
// `ReadOnlyStringSpan` constructor is a BUILT-IN record with no user
// ConstructorSymbol, so it took that conservative branch (the
// resolved-user-constructor relaxation of #2023 and the builtin-exception
// relaxation of #2324 do not reach it), the source local `w` was excluded
// from `stringReleaseEligible_`, and its scope-end `bz_string_release` never
// fired.
//
// NET EFFECT: the source string was never released at all — one heap string
// (two allocations) leaked per construction. (The `AsSpan`/`Slice` factories
// take the #1430 plain-string-parameter relaxation, so THEIR source local IS
// released at scope end — that is why the issue calls the `AsSpan`-built span
// the clean twin.)
//
// THE FIX (the constructor path only; the `Slice`/`AsSpan` path is NOT
// touched):
//
//   * the escape walk admits the built-in `ReadOnlyStringSpan`
//     constructor's `Source` argument as a CALL-BOUNDED BORROW — the same
//     treatment #2023 gives a resolved constructor's plain `string`
//     parameter — so the source local's own scope-end release fires;
//   * the admission is NARROW: it applies ONLY to a construction site that
//     (a) initializes a LOCAL DECLARATION and (b) is proven non-escaping by
//     the SAME conservative escape walk the string locals use (the
//     `ctorSpanBorrowDecl_` map). A span built in a return value, in an
//     argument, or in an assignment is not admitted, and neither is a span
//     that escapes (returned, captured by a closure, stored into a
//     field/collection, or passed as an argument): for those the source
//     keeps the conservative escape, so it is NOT released (a leak — the
//     safe direction) and the span stays valid, as §3.3 requires ("the span
//     may sit in fields, cross `await`, and outlive any scope").
//
// THE OWNERSHIP TRAP these rows exist for: a span BORROWS its source, so the
// source may be released while the span is alive ONLY when the span dies no
// later than the source — which is exactly what the non-escaping proof buys.
// Row 3 pins the first direction (the SOURCE is read AFTER the span died)
// and row 4 the second (the SPAN outlives the source's scope): a naive
// relaxation that released the source unconditionally would turn row 4 into
// a heap-use-after-free, and a relaxation that minted a +1 in the
// constructor would leak that +1 at every call site with no owner to
// discharge it.
//
// THE ORACLES. `runAotChecked` instruments the GENERATED program with
// AddressSanitizer and LeakSanitizer and treats ANY sanitizer finding as
// `ran=false` — so row 1 (the loop-amplified leak oracle) is red on the
// pre-fix compiler and green with the fix. `runAotCheckedLenient` is ASan
// WITHOUT LeakSanitizer: row 4's program carries a KNOWN, DOCUMENTED residual
// leak (the source of a span that ESCAPES is deliberately not released — the
// conservative direction), so row 4 uses the crash-freedom oracle: it pins
// that the shape does not CRASH (a use-after-free IS caught by ASan without
// LSan).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The CRASH-FREEDOM oracle (detect_leaks=0): proves no use-after-free /
// double-free / out-of-bounds, while tolerating an EXPECTED, still-safe
// leak. Used by row 4, whose program's residual leak is documented above.
bool didNotCrash(const std::string& program) {
    auto r = runAotCheckedLenient(program);
    return r.ran;
}

// The issue's own repro, verbatim except for the explicit `Main(): int`
// (so the exit code is a determinate 0) and the trailing `return 0;`.
// `for var i in 0 .. 100` is INCLUSIVE (§16.3), so the loop runs 101
// times and the accumulated `sp.Length` is 303 — the value the issue's
// repro prints on the pre-fix compiler too, so the row pins the LEAK and
// not a behaviour change.
const char* kIssueRepro =
    "module Conf; "
    "public class Program "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0 .. 100 do "
    "      var w := \"wide \xc3\xa9\xc3\xa8 \" + i.ToString(); "
    "      var sp := new ReadOnlyStringSpan(w, 1, 3); "
    "      t := t + sp.Length; "
    "    end for; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// ===========================================================================
// ROW 1 — THE LEAK ORACLE (the issue's own repro, 101 iterations).
// Red on the pre-fix compiler (LSan reports 4334 B in 202 allocations) and
// green with the fix. The loop is amplified 101× because a ONE-SHOT leak
// repro can hide under the sanitizer's noise (LANE_RULES §8).
// ===========================================================================
BEZEL_TEST(readonlystringspan_ctor_leak_8162,
           ctor_leak_no_leak_spec_3_3) {
    auto r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("303\n"));
}

// ===========================================================================
// ROW 2 — VALUE CORRECTNESS (the plain oracle, no sanitizer). The fix
// changes the DISPOSAL of the source, never the value the program observes:
// the accumulated `sp.Length` is still 303 and the exit code is still 0.
// ===========================================================================
BEZEL_TEST(readonlystringspan_ctor_leak_8162,
           ctor_value_correctness_plain_oracle) {
    auto r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("303\n"));
}

// ===========================================================================
// ROW 3 — MUST NOT USE-AFTER-FREE: the SOURCE is read AFTER the span has
// died. The span is declared in an INNER scope and the source in an OUTER
// one, so the span dies first and the source is still read afterwards. The
// source's own scope-end release fires at the OUTER scope's end — after
// this read — so the source is alive when it is printed.
// A fix that released the SOURCE at the span's scope end (or that freed the
// string twice) would turn this row into a heap-use-after-free / a
// double-free here.
// ===========================================================================
BEZEL_TEST(readonlystringspan_ctor_leak_8162,
           source_read_after_span_death_no_uaf_spec_3_3) {
    const char* prog =
        "module Conf; "
        "public class Program "
        "  public static method Main(): int "
        "    var w := \"hello\" + \"world\"; "
        "    do "
        "      var sp := new ReadOnlyStringSpan(w, 1, 3); "
        "      Console.WriteLine(sp[0]); "
        "    end; "
        "    Console.WriteLine(w); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // "e" — the span's first code point (Source[Start + 0] = "helloworld"[1]).
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("e\nhelloworld\n"));
}

// ===========================================================================
// ROW 4 — MUST NOT USE-AFTER-FREE: the SPAN outlives the inner scope that
// holds the SOURCE. The callee builds the span over a source that dies with
// the callee's scope, and hands the span back to the caller, which reads it
// afterwards. §3.3: "the span may ... outlive any scope" — so the source
// must NOT be released while an escaped span still points at it.
//
// This is the row that keeps the fix honest: a construction site that is not
// a local-declaration initializer is NOT admitted by the fix's escape-walk
// relaxation, so the callee's source keeps the conservative escape (it is
// not released — a documented, deliberate leak, the safe direction) and the
// span stays valid. A relaxation that released the source unconditionally
// would turn this read into a heap-use-after-free.
//
// The crash-freedom oracle (ASan without LSan) is used because this
// program's residual leak is deliberate and unchanged in magnitude by this
// fix (the pre-fix compiler leaked the very same string here, through the
// source's own suppressed scope-end release).
// ===========================================================================
BEZEL_TEST(readonlystringspan_ctor_leak_8162,
           span_outlives_source_scope_no_uaf_spec_3_3) {
    const char* prog =
        "module Conf; "
        "public class Program "
        "  public static method MakeSpan(): ReadOnlyStringSpan "
        "    var w := \"hello\" + \"world\"; "
        "    return new ReadOnlyStringSpan(w, 1, 3); "
        "  end MakeSpan; "
        "  public static method Main(): int "
        "    var sp := MakeSpan(); "
        "    Console.WriteLine(sp[0]); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // "e" — the span's first code point, read AFTER the source's scope ended.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("e\n"));
}

// ===========================================================================
// ROW 5 — THE `AsSpan` TWIN IS NOT TOUCHED (the brief's "do not touch the
// Slice path"). The `AsSpan` factory already released its source at scope
// end (it takes the #1430 plain-string-parameter relaxation), so the
// factory-built span is leak-free BOTH before and after this fix. This row
// is the recorded control that the fix's constructor-path change did not
// widen into the factory path: if the constructor-path change had leaked
// into the factory path, this row would report a leak.
// ===========================================================================
BEZEL_TEST(readonlystringspan_ctor_leak_8162,
           asspan_twin_still_clean_spec_3_3) {
    const char* prog =
        "module Conf; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 .. 100 do "
        "      var w := \"wide \" + i.ToString(); "
        "      var sp := w.AsSpan(1, 3); "
        "      t := t + sp.Length; "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("303\n"));
}
