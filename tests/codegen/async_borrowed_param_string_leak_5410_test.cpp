// Codegen / refcounting — issue #5410.
//
// WHAT LEAKS. An async method whose body ASSIGNS to a `string` parameter
// (`s := s + "-x"`) mints a fresh heap string per resume activation and never
// releases it. Measured on the repro below (10 awaited calls, ASan/LSan):
// 390 bytes in 20 allocations — 32 B `bz.string` header + 7 B width-1 payload
// per call — with allocating frame #1 `bz$async$M.C$Take$Resume`.
//
// IT IS NOT THE BORROWED VALUE. The frame-lifetime pin #5262-ff added (retain
// the borrowed `string` formals into the frame at entry, release at settlement)
// is already balanced, and the leak's ALLOCATING stack is inside the resume,
// which is where `s + "-x"` runs — the leaked object is the CONCAT RESULT, not
// the caller's `h.Name`. The synchronous twin of this exact body
// (`static method Take`) does not leak, which is what isolates the defect to
// the async routing rather than to the reassignment itself.
//
// ROOT CAUSE. retainAssignedStringParams mints the per-activation +1 that
// licenses lowerStringLocalReassign's drop-on-overwrite release. Its scope-end
// release of the parameter's FINAL value is gated on `armScopeEndRelease`,
// which defaults to FALSE. Both existing callers that pass true are synchronous
// (Setup.cpp:10760, the method body; Calls2.cpp:17140, the generic
// specialization) and both rely on their body's ROOT `lowerScopedBlock` to
// drain the queued Disposal out of `pendingChainOutDisposals_`. An async body is
// lowered through `lowerBody`, which never reaches that drain — so even with
// the gate opened, the Disposal would be discarded when the resume's
// `disposeStack_` frame pops. The fix opens the gate in emitAsyncMethod and
// adds the matching drain.
//
// Spec: §28.9 "Runtime-managed types" — a `string`'s reference is reclaimed
// when no reference to it remains; §28.7 — reassignment releases the displaced
// value, and the slot's own final value is released at scope end; §4.6
// "`string`"; §30.1 "`async` methods" — the task "settles" at a terminal state,
// which is where this release belongs. Spec revision v0.313.
//
// THE REGRESSION: `BEZEL_EXPECT_TRUE(r.ran)` is the whole oracle — under
// BEZEL_AOT_DETECT_LEAKS=strict a leak fails the row (r.ran == false) and a
// clean run passes, and `r.exitCode == 60` pins the observable value so a
// program broken by the fix cannot pass vacuously.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
static bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// A holder whose string field is the borrowed source. Built at runtime (not a
// literal, which would be an rc sentinel and make the oracle vacuous).
//
// fix-forward #5410: NO `//` comment may appear inside this string. The repro
// is ONE line, so a `//` comments out everything after it — including `end
// Take`, `end Main` and `end C` — and the program then fails to PARSE. That is
// not a hypothetical: the row as first written carried three in-string comments
// and its own `[ RUN ]` never compiled anything. It reported `ran == false` for
// a parse error (`expected 'end'`), which the strict-leak assertion read as a
// leak. Prose about the shape belongs in this file's C++ comments, not in the
// source text.
const char* kAsyncAssignedStringParamLeakRepro =
    "module M; class Holder "
    "  public Name: string := \"\"; "
    "end Holder; "
    "class C "
    "  private static async method Take(s: string, i: int): Task<int> "
    "    s := s + \"-x\"; "
    "    return s.Length; "
    "  end Take; "
    "  public static async method Main(): Task<int> "
    "    using var h := new Holder(); "
    "    var seed := 7; "
    "    h.Name := $\"hn-{seed}\"; "
    "    var total := 0; "
    "    for var i in 1..10 do "
    "      total := total + await C.Take(h.Name, i); "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// fix-forward #5410 (§30.1 / §4.6 / §28.9, v0.313): the borrowed string
// parameter pin now releases at Resume START, before the assignment can clobber
// the frame field. Before the fix, the teardown release reads the clobbered
// frame field and releases the NEW value (s + "-x") instead of h.Name — one
// reference leaks per resume activation. After the fix, h.Name is released at
// Resume start and the new string is released by the drop-on-overwrite, so
// the borrowed reference is correctly balanced.
BEZEL_TEST(codegen_async_borrowed_param_string_leak_5410,
           async_assigned_string_param_borrowed_ref_balanced_spec_30_1) {
    // Run under ASan + LSan. Before the fix: the borrowed string reference
    // (h.Name) is never released because the teardown release reads the
    // clobbered frame field and releases the new value instead. After the fix:
    // clean — no memory errors, no leaks.
    JitResult r = runAotChecked(kAsyncAssignedStringParamLeakRepro);
    if (wasSkipped(r)) return;
    // BEZEL_EXPECT_TRUE(r.ran) asserts no ASan finding (leak, UAF, OOB) in
    // strict mode; in report mode it is informational. Before the fix this
    // fires (r.ran == false, LSan leak detected). After the fix it passes.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}
