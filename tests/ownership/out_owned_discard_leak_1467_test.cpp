// Codegen / runtime — disposing an `out owned` (class-typed) value that a call
// discards with `out _` (spec §28.3 / §17.13; issue #1467).
//
// §22.4 (v0.177): "`out _` (v0.160). The discard form applies to both parameter
// kinds: against `out T` the received value is simply dropped (§17.13); against
// `out owned T` the discarded value is an unbound owned temporary disposed at the
// end of the full statement (§28.3)."
//
// THE BUG (#1467). `buildOrderedArgs` handled an `out _` discard by allocating a
// throwaway scratch slot and passing its address (correct for a value-typed `out
// T`), but did so UNCONDITIONALLY — with no check for `out owned`. For an `out
// owned` class parameter the callee writes a fresh OWNED heap object into that
// scratch slot; with the value bound to nothing, §28.3's unbound-owned-temporary
// disposal was skipped entirely. The object's destructor never ran AND its block
// leaked — LeakSanitizer reported a DIRECT leak in the producing method.
//
// THE FIX. The discard branch now records the scratch slot for an `out owned`
// class/interface parameter (null-initialized); right after the call, on the
// normal-return path, the slot's value is disposed (dynamic vtable Dispose +
// reclaim) null-guarded and exactly once — the §28.3 end-of-statement disposal of
// an unbound owned temporary. A real `out owned var x` binding is not a discard,
// so its ownership transfer to the caller is untouched (no double-dispose).
//
// OBSERVABILITY. The leak is a DIRECT leak (nothing keeps the object reachable),
// so runAotChecked's LSan flags it: WITHOUT the fix `r.ran` is false. As a second,
// spec-aligned observable the class destructor prints a marker — absent without
// the fix (the destructor never runs), present exactly once with it. runAotChecked
// also guards the with-fix path against a double-free the disposal could introduce
// (dispose-exactly-once net rides on r.ran).

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// runAotChecked returns ran=false with a "runAotChecked: SKIP" diagnostic. clang
// is required to build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// #1467 repro (from the issue). TryMake produces `new Res(5)` through an
// `out owned r: Res?` parameter; the caller discards it with `out _`. §28.3
// requires the discarded owned temporary to be disposed at end-of-statement —
// so the destructor ("RES-DISPOSED-5") must run exactly once and nothing leaks.
const char* kOutUnderscoreDiscardsOwned =
    "module M; "
    "class Res : IDisposable "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"RES-DISPOSED-\" + Id.ToString()); "
    "  end destructor; "
    "end Res; "
    "class W "
    "  public static method TryMake(out owned r: Res?): bool "
    "    r := new Res(5); "
    "    return true; "
    "  end TryMake; "
    "  public static method Main(): int "
    "    var ok := TryMake(out _); "
    "    Console.WriteLine(\"ok=\" + ok.ToString()); "
    "    return 0; "
    "  end Main; "
    "end W;";

// Regression: the REAL binding `out owned var x` must still TRANSFER ownership to
// the caller (x is usable) and dispose exactly once at scope end — NOT be disposed
// by the discard path. The fix records ONLY discard slots, so x is untouched: a
// wrong dispose here would double-free (r.ran false under ASan).
const char* kOutOwnedVarBindingTransfers =
    "module M; "
    "class Res : IDisposable "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"RES-DISPOSED-\" + Id.ToString()); "
    "  end destructor; "
    "end Res; "
    "class W "
    "  public static method TryMake(out owned r: Res?): bool "
    "    r := new Res(5); "
    "    return true; "
    "  end TryMake; "
    "  public static method Main(): int "
    "    var ok := TryMake(out var x); "
    "    if x <> null then Console.WriteLine(\"got=\" + x.Id.ToString()); end; "
    "    Console.WriteLine(\"ok=\" + ok.ToString()); "
    "    return 0; "
    "  end Main; "
    "end W;";

}  // namespace

// =====================================================================
// #1467 — `out _` against an `out owned` class param disposes the owned
// value exactly once (§28.3 / §17.13); no leak.
// =====================================================================

BEZEL_TEST(ownership_out_owned_discard_1467,
           out_underscore_discards_owned_disposes_spec_28_3_17_13) {
    // spec §28.3 / §17.13, v0.177: `out _` against `out owned T` makes the
    // discarded value an unbound owned temporary disposed at end-of-statement.
    // WITHOUT the fix the produced Res(5) is neither disposed nor reclaimed — a
    // direct leak flips r.ran to false and "RES-DISPOSED-5" never appears.
    JitResult r = runAotChecked(kOutUnderscoreDiscardsOwned);
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip, not a failure

    // Clean run under LSan (the #1467 direct leak would flip this false) and the
    // disposal introduced no double-free / use-after-free.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);

    // Load-bearing #1467 assertion: the discarded owned value WAS disposed,
    // exactly once (§28.3). Absent the fix this count is 0.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED-5"), 1);

    // The call's bool return is still observed correctly (ok=True in the issue).
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ok=True") != std::string::npos);

    // The destructor runs BEFORE the next statement's output — the temporary is
    // disposed at the end of the discarding statement, not deferred past it.
    const std::size_t disp = r.stdoutText.find("RES-DISPOSED-5");
    const std::size_t okln = r.stdoutText.find("ok=True");
    BEZEL_EXPECT_TRUE(disp != std::string::npos);
    BEZEL_EXPECT_TRUE(okln != std::string::npos);
    BEZEL_EXPECT_TRUE(disp < okln);
}

// =====================================================================
// Regression — a real `out owned var x` binding still transfers ownership
// to the caller and disposes exactly once (the discard fix must not touch it).
// =====================================================================

BEZEL_TEST(ownership_out_owned_discard_1467,
           out_owned_var_binding_still_transfers_spec_28_3) {
    // §22.4 / §28.3: `out owned var x` binds an inline owned local that the
    // caller owns; it is usable and auto-dropped exactly once at scope end. The
    // #1467 fix records ONLY discard slots, so this transfer is unaffected — a
    // stray dispose of x would double-free (r.ran false).
    JitResult r = runAotChecked(kOutOwnedVarBindingTransfers);
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // x owned the produced value (usable) ...
    BEZEL_EXPECT_TRUE(r.stdoutText.find("got=5") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ok=True") != std::string::npos);
    // ... and it was disposed EXACTLY once (no double-dispose from the discard
    // path; no leak).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "RES-DISPOSED-5"), 1);
}
