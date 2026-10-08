// Regression tests for issue #5145 — §28.3 *A `ref` or plain `out` location
// outlives the call* (spec v0.330; derives K2.3 + K2.4 with K2.2's union;
// issue #4819's call-site leg).
//
// A `ref` or plain `out` argument location whose type can carry a borrow is
// CALLER storage the callee may fill with any borrow the call carries: the
// receiver, every plain (non-`owned`, non-`scoped`) argument, and every other
// `ref` argument's current value. The caller must apply to each such location
// the admission the DIRECT assignment into it would apply, and a violation is
// rejected at the call site. The rule reads only the callee's SIGNATURE — "a
// callee that stores nothing is checked as one that stores everything" — so the
// check is driven for every CallExpr and needs no callee-body analysis.
//
// MEASURED on the parent (r23-c28 / 6d433c29), `scripts/agent_check.sh
// --probe --asan` on the issue's canonical repro: the program compiled with
// rc=0 and NO diagnostic, and the emitted native binary died with
//   `AddressSanitizer: heap-use-after-free ... in bz$M.Widget$Name`
// on the `keep.Name()` read after `t`'s `do` block disposed `t`. The callee's
// own `slot := x` store IS rejected INSIDE the callee (§28.3's callee-side
// sentence, enforced by `checkBorrowStoreEscape`'s `Dest::OutParam` leg) — what
// was missing is the CALLER's half.
//
// The reject rows below pin the rule's several shapes (a `ref` local, the
// plain-`out` leg, the receiver as a source); the accept rows pin that the
// rule does NOT fire where the spec says it must not: an `out _` discard names
// no location, a source that outlives the location is fine, a value-typed
// location copies rather than borrows, an `out owned` argument transfers rather
// than borrows, and a function-typed location is exempt from the caller-side
// union (v0.327).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// A heap-owning class so the borrow is a real heap reference (a value type
// would be copied, and §28.3's rule would not apply to it at all).
const char* kPrelude5145 = R"BZL(
module M;
class Widget : IDisposable
  private _name: string;
  public constructor(name: string) _name := name; end constructor;
  public method Name(): string => _name;
  public method Put(ref slot: Widget, x: Widget): () slot := x; end Put;
  public static method Swap(out slot: Widget, x: Widget): () slot := x; end Swap;
  public method Store(ref slot: Widget): () slot := this; end Store;
  destructor end destructor;
end Widget;
)BZL";

// The issue's canonical call-site refusal. `t` is an owned scope-bound local
// disposed at the end of its `do` block; `keep` is declared in the enclosing
// block, so the borrow the call stores into it outlives its source — exactly
// the direct assignment `keep := t`, which the compiler already rejects.
std::string putRefLocal5145(const char* decl) {
    return std::string(kPrelude5145) + R"BZL(
class Program
  public static method Main(): int
    using var other := new Widget("other");
    var keep := other;
    do
      using var t := new Widget("t");
)BZL" + decl + R"BZL(
    end;
    Console.WriteLine($"keep={keep.Name()}");
    return 0;
  end Main;
end Program;
)BZL";
}

const char* kNeedle5145 =
    "would be stored into `keep` by this call, and outlive its source";

}  // namespace

// ===========================================================================
// REJECT rows — the call-site leg of §28.3's rule.
// ===========================================================================

// The issue's literal repro: `Put(ref keep, t)`.
BEZEL_TEST(ownership_ref_out_location_outlives_call_5145,
           spec_28_3_ref_local_put_rejected) {
    auto r = runAotChecked(putRefLocal5145("      Put(ref keep, t);"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle5145));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`t`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "§28.3 *A `ref` or plain `out` location outlives the call*"));
}

// The plain-`out` leg. §28.3 names "a `ref` or plain `out` argument location"
// as one kind, so the same store through `out` is the same violation — this
// row is what makes the `out` half of the modifier table non-vacuous.
BEZEL_TEST(ownership_ref_out_location_outlives_call_5145,
           spec_28_3_plain_out_local_rejected) {
    auto r = runAotChecked(putRefLocal5145(
        "      Program.Swap(out keep, t);"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle5145));
}

// The receiver is one of the pooled sources in its own right ("the receiver
// and every plain argument"), so `obj.Store(ref keep)` — where the callee
// stores `this` — is the same violation reached through the receiver rather
// than through an argument.
BEZEL_TEST(ownership_ref_out_location_outlives_call_5145,
           spec_28_3_receiver_source_rejected) {
    auto r = runAotChecked(std::string(kPrelude5145) + R"BZL(
class Program
  public static method Main(): int
    using var other := new Widget("other");
    var keep := other;
    do
      using var obj := new Widget("obj");
      obj.Store(ref keep);
    end;
    Console.WriteLine($"keep={keep.Name()}");
    return 0;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNeedle5145));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "the receiver `obj`"));
}

// ===========================================================================
// ACCEPT rows — the rule must NOT fire where the spec says it must not.
// ===========================================================================

// `out _` "names no location and takes no part" (§28.3). Without this row a
// check that pooled the borrow into the discard would reject a program the
// spec admits.
BEZEL_TEST(ownership_ref_out_location_outlives_call_5145,
           spec_28_3_out_discard_exempt_accepted) {
    auto r = runAotChecked(std::string(kPrelude5145) + R"BZL(
class Program
  public static method Discard(out slot: Widget, x: Widget): ()
    slot := x;
  end Discard;
  public static method Main(): int
    do
      using var t := new Widget("t");
      Discard(out _, t);
    end;
    return 0;
  end Main;
end Program;
)BZL");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Every source outlives the location, so the borrow is admissible and the
// direct assignment `keep := t` would be admitted too. This is the control
// that proves the check is a scope COMPARISON and not a blanket refusal of
// every `ref`/`out` argument.
BEZEL_TEST(ownership_ref_out_location_outlives_call_5145,
           spec_28_3_source_outlives_location_accepted) {
    auto r = runAotChecked(std::string(kPrelude5145) + R"BZL(
class Program
  public static method Main(): int
    using var t := new Widget("t");
    using var other := new Widget("other");
    var keep := other;
    keep.Put(ref keep, t);
    Console.WriteLine($"keep={keep.Name()}");
    return 0;
  end Main;
end Program;
)BZL");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("keep=t") != std::string::npos);
}

// A VALUE-typed `ref` location copies an int into the location, so it carries
// no borrow and nothing can dangle there. Pooling a borrow against it would be
// a false rejection of an ordinary integer in-out parameter.
BEZEL_TEST(ownership_ref_out_location_outlives_call_5145,
           spec_28_3_value_typed_ref_location_accepted) {
    auto r = runAotChecked(R"BZL(
module M;
class Program
  public static method Bump(ref n: int, by: int): () n := n + by; end Bump;
  public static method Main(): int
    var n := 1;
    do
      using var t := new Program();
      Bump(ref n, 2);
    end;
    Console.WriteLine($"n={n}");
    return 0;
  end Main;
end Program;
)BZL");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("n=3") != std::string::npos);
}

// A FUNCTION-TYPED location is exempt from the caller-side union (v0.327):
// the callee may fill it only capture-free, so on the call's normal return an
// `out` carries the empty set and a `ref` the set it carried before, and the
// call's borrows need no admissibility check against it. The row is written
// against the SHAPE (an outer-scope function-typed `out` recipient filled from
// an inner scope, which is the arrangement a blanket union check would reject)
// and asserts ACCEPTANCE ONLY — via `runAot`, not `runAotChecked`.
//
// Deliberately not a checked-disposal row: MEASURED on r23-c28 (6d433c29) the
// closure a callee allocates into a `ref` slot is never disposed, which is a
// SEPARATE pre-existing leak, present identically with and without this fix
// (ASan `96 byte(s) leaked in 2 allocation(s)`, allocated in
// `bz$M.Program.Fill`, on BOTH trees). Pinning it here would make this row fail
// for a defect #5145 does not claim to fix; the leak is out of scope here.
BEZEL_TEST(ownership_ref_out_location_outlives_call_5145,
           spec_28_3_function_typed_location_exempt_accepted) {
    auto r = runAot(R"BZL(
module M;
class Program
  public static method Fill(out f: () -> int, v: int): () f := lambda(): int => 7; end Fill;
  public static method Main(): int
    var f: () -> int := lambda(): int => 7;
    do
      using var t := new Program();
      Fill(out f, 3);
    end;
    Console.WriteLine($"filled={f()}");
    return 0;
  end Main;
end Program;
)BZL");
    // The row's claim is that the call-site check does NOT fire: the program
    // compiles clean. Asserted positively on the ABSENCE of the diagnostic and
    // on a successful native run — not on an absent-crash oracle.
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        !diagnosticMessageContains(r, "would be stored into"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("filled=7") != std::string::npos);
}