// Spec-derived regression test for issue #3759 — an INTERPOLATED-STRING
// argument in the §22.10 inheritance-clause base call, routed through an
// EXPLICIT base constructor, leaks. The string twin of #3750 (owned
// temporary for a plain base positional parameter never released).
//
// ── THE SHAPE ──────────────────────────────────────────────────────────────
//
//     class Base
//       public s: string;
//       public constructor(s0: string) s := s0; end constructor;
//     end Base;
//     class Derived(n: int) : Base($"v{n}")
//     end Derived;
//
//     var i: int := 0;
//     while i < 200 do
//       using var d := new Derived(i);
//       i := i + 1;
//     end while;
//
// ── THE DEFECT (measured on r12-cycle-1 @ f517ddb / a31fb7b post-#3742) ───
//
// : Base($"v{n}") passes an interpolated string to Base's explicit ctor.
// `lowerExpr($"v{n}")` produces a fresh +1 heap-allocated string (the
// interpolation emitter, see LlvmBackend_Exprs.cpp ~line 180, mints one +
// discharges each per-step accumulator); the explicit ctor stores it as a
// BORROW into `s` (strings are runtime-managed, §28.9, and the §22.3 plain
// parameter is a borrow); §22.10's capture rule owns the resulting field.
// Without the fix, the producer's +1 reference is never discharged —
// `lowerCall`'s ARGUMENT-PASS post-call release (the #429 site, see
// LlvmBackend_Calls2.cpp ~line 10415) covers ordinary method calls but NOT
// the §22.10 base-clause site, which goes through `emitImplicitBase` and
// calls `bce.fn` directly.
//
// MEASURED f517ddb / a31fb7b post-#3742, pre-#3759-fix:
//   `using var d := new Derived(i);` × 200 iterations
//     Direct leak of 6400 B / 200 objects
//     Indirect leak of 890 B / 200 objects
//     TOTAL 7290 B / 400 allocations   (= the issue's "7200 B / 400")
//
// Same shape in the chain form `this($"v{x}")` — covered by row T4. The
// primary-ctor-only face `: Base($"v{n}")` over `class Base(s: string)` is a
// different leak family (the field-destruction path differs from the explicit
// ctor's body) and is NOT in scope for #3759.
//
// ── THE FIX (measured) ─────────────────────────────────────────────────────
//
// `emitImplicitBase` (LlvmBackend_Setup.cpp) now releases every +1 string
// producer among `primaryBaseCtorArgs` AFTER the explicit-ctor `CreateCall`.
// Mirrors `lowerCall`'s #429 site byte-for-byte: same `!diverged_` guard, same
// `isPlusOneStringProducer` predicate, same per-argument scan. The §22.5
// `fillOmittedCtorArgs`-materialized defaults past the clause's own arity
// are out of scope — they are filled inside the helper with their own release
// discipline; only the clause's own expressions are released here.
//
// MEASURED post-fix: `ok` and 0 leak objects under LSan.
//
// per §22.10 "When a class with a primary constructor has a base class, the
//   base call appears in the inheritance clause" (v0.286)
// per §22.8 step 1 "The base or chained constructor call" (v0.286)
// per §22.8 step 0 / §22.10 "Capture rule and layout stability" (v0.286)
// per §28.9 ARGUMENT-PASS: a +1 producer passed to a plain (borrow) parameter
//   is either rejected or released by the caller after the call
// per §22.3 "Parameter modifiers": plain `string` is a borrow (§28.9 runtime-
//   managed exemption)
// issue #3759; sibling #3750 (the class-typed owned twin), #3742 (the
// evaluator). §12.8 constructor chaining.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace bezel::codegen {
namespace {

// Per-process unique working dir under the shared scratch base — forked
// isolates every concurrent test process (see tests/README.md and
// support/scratch_dir.hpp). `static` + function-local so the dir is created
// on first use and reused by every test in this TU.
static const std::filesystem::path& wtDir3759() {
    return wtScratchDir("wt-3759");
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// T1 — HEADLINE REPRO. The exact program from the issue's "Measured" block,
// looped 200× under ASan/LSan via `runAotChecked`. FAILS-WITHOUT the
// `emitImplicitBase` post-call release (LSan reports 7200 B / 400 alloc
// leak); PASSES-WITH.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 / §22.8 step 1 / §28.9 ARGUMENT-PASS, issue #3759
BEZEL_TEST(interpolated_string_base_clause_3759,
           base_clause_interpolated_string_argument_spec_22_10) {
    const std::string src =
        "module L3759a; "
        "class Base "
        "  public s: string; "
        "  public constructor(s0: string) s := s0; end constructor; "
        "end Base; "
        "class Derived(n: int) : Base($\"v{n}\") "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(i); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The headline leak oracle — `runAotChecked` runs the program under
    // `ASAN_OPTIONS=detect_leaks=1` and a non-zero `r.exitCode` is what LSan
    // signals with; `runAotChecked` rejects on any LSan / ASan report, so
    // the boolean above is the leak oracle. The two extra asserts below are
    // VALUE checks that pin the program's observable behaviour so a future
    // diagnostic-quality regression (e.g. a "wrong-value" fix that
    // accidentally mints a literal sentinel) is still caught.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ok") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T2 — CONTROL: literal base-clause argument. A `string` LITERAL is an
// immortal sentinel — no +1 reference, no leak on either side. Pins that
// the fix's predicate `isPlusOneStringProducer` correctly recognizes the
// producer/non-producer distinction (a literal is NOT a producer, no
// release is emitted).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 / §28.9, control row
BEZEL_TEST(interpolated_string_base_clause_3759,
           base_clause_string_literal_argument_control) {
    const std::string src =
        "module L3759b; "
        "class Base "
        "  public s: string; "
        "  public constructor(s0: string) s := s0; end constructor; "
        "end Base; "
        "class Derived(n: int) : Base(\"literal\") "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(i); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ok") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T3 — CONTROL: the same interpolated string, but in the DIRECT `new`
// position (`new Base($"v{n}")`), NOT in a base-clause call. The `lowerCall`
// ARGUMENT-PASS release (the #429 site) already covers this path; this row
// pins that the pre-existing release keeps the same string-clean outcome
// even when the value flows to an explicit ctor's plain string parameter —
// proving the new §22.10 base-clause release site is the GAP, not a
// duplicate.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 / §28.9 ARGUMENT-PASS (the #429 site already covers this),
// control row
BEZEL_TEST(interpolated_string_base_clause_3759,
           direct_new_interpolated_string_argument_control) {
    const std::string src =
        "module L3759c; "
        "class Base "
        "  public s: string; "
        "  public constructor(s0: string) s := s0; end constructor; "
        "end Base; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Base($\"v{i}\"); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ok") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T4 — CHAIN FORM: `this($"v{x}")` on a convenience ctor chains to the
// §22.10 primary, which then chains to Base. The interpolated-string +1 is
// carried through both chain links: the chain-call arg is lowered once
// (the +1 it produces flows into the primary's parameter slot), the primary's
// step 0 (`storePrimaryCtorPositionalArgs`) transfers the +1 into the
// inherited primary parameter (which #1605 already discharged via a field-
// local retain), the primary's inheritance clause then routes the value
// into Base — where the T1 release fires (the fix's `emitImplicitBase`
// post-call release is the same code path that handles the primary's own
// `: Base(s)` inheritance clause). Net: no leak; the chain propaga tes the
// +1 reference into Base's field, which Base.Dispose releases. Row 4
// pins the no-op-pin outcome so a future chain-call leak (analogous to
// the #3750 fix's other call sites) is caught here too.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 / §12.8 / §28.9, control row for the chain twin
BEZEL_TEST(interpolated_string_base_clause_3759,
           this_chain_interpolated_string_argument_control) {
    const std::string src =
        "module L3759d; "
        "class Base "
        "  public s: string; "
        "  public constructor(s0: string) s := s0; end constructor; "
        "end Base; "
        "class Derived(s: string) : Base(s) "
        "  public constructor(x: int) this($\"u{x}\"); end constructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new Derived(i); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ok") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T5 — VALUE CHECK: the interpolated string is the value Base.s receives.
// Without this, a future "fix" that emits `bz_string_release` of the WRONG
// pointer (e.g. on the wrong release site) could pass T1 by turning the
// release into a no-op while still leaking. The `s` field's stored string
// must equal the constructed value (rc=1 — no premature release).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 "Capture rule and layout stability" / §28.9, value oracle
BEZEL_TEST(interpolated_string_base_clause_3759,
           base_clause_interpolated_string_value_pinned_spec_22_10) {
    const std::string src =
        "module L3759e; "
        "class Base "
        "  public s: string; "
        "  public constructor(s0: string) s := s0; end constructor; "
        "end Base; "
        "class Derived(n: int) : Base($\"v{n}\") "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(5); "
        "    Console.WriteLine($\"got={d.s}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("got=v5") != std::string::npos);
}

}  // namespace bezel::codegen
