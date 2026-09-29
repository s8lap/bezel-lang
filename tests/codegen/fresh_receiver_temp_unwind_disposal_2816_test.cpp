// Regression tests for #2816 — the EXCEPTION-PATH half of §28.3's
// unbound-owned-temporary rule for a fresh `new` RECEIVER temporary.
//
// WHAT THE DOCUMENT REQUIRES.  Spec §28.3 "Local variables, ownership, and
// borrows", subsection *Unbound owned temporaries* (v0.240), immediately after
// the sentence #1039/#1053/#2751/#2814 implement:
//
//     When the full expression is instead exited by an **exception** — an
//     operand or argument evaluated later throws before the temporary's
//     transfer or consumption point — every already-produced, not-yet-consumed
//     unbound owned temporary is disposed during unwinding by the same implicit
//     `try`/`finally` mechanism as named locals (*Exception-path discharge*
//     above; [K3.15]/[K8.1]): each temporary's disposal is established at its
//     production, so the pass runs in reverse order of production on the
//     establishment-ordered cleanup stack
//
// THE DEFECT.  The whole `disposeFreshNewReceiverTemp` family emitted its
// reclaim INLINE after the accessor — guarded by `!diverged_` and placed after
// `emitPendingCheck()`, i.e. in the normal-return continuation block.  A getter,
// indexer, or callee that threw simply branched past it, and nothing registered
// the temporary with the unwind machinery, so it leaked.  All four spellings
// shared the placement: the field read and method call (#1039), the four
// property-getter paths (#2751) and the five indexer-getter paths (#2814).
//
// MEASURED pre-fix on b0fa41a with a live-object oracle that isolates the two
// edges in one program — twenty iterations that throw from the accessor, then
// twenty that do not, then `return Src._live`:
//
//     exit 20      (320 byte(s) leaked in 20 allocation(s) under LSan)
//
// The twenty normal-path receivers were reclaimed (#2751 working) and all twenty
// throw-edge receivers leaked — a clean discriminator that rules out "the
// reclaim never runs" and pins the defect to the unwind edge.
//
// THE FIX.  `registerFreshNewReceiverTempUnwind` establishes the temporary's
// disposal AT ITS PRODUCTION as a null-guarded `unwindOnly` Disposal on the
// current scope — the mechanism §28.3 names, and the same one #993 uses for an
// owned `new` ARGUMENT temporary — and the existing inline reclaim discharges it
// by NULLING the slot before it frees the block.  Registration and reclaim share
// one eligibility predicate (`freshNewReceiverTempEligible`), so a temporary can
// never be registered by one and declined by the other.
//
// THE ORACLE IS A LIVE-OBJECT COUNTER, NOT "NO LEAK".  This fix has a real
// double-free direction — registering on the cleanup stack while the inline
// reclaim also fires would dispose twice — and LSan cannot see that.  Every
// program below folds the live count into the exit code and every case runs
// under generated-code ASan+LSan, so an over-release faults and an under-release
// leaks.  Every loop is amplified to 20 iterations: a one-shot `Main` is not a
// reliable LSan oracle in this tree (a leftover register pointer makes a leaked
// block read as *reachable*).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

// (`runAot` is no longer used here: the last plain-oracle row — the `string`
// result EXCLUSION control — moved onto the ASan+LSan `run()` helper when
// #4552 reversed it.)
using bezel::test::runAotChecked;

namespace {

// ASan + LSan on the generated program.  A leak, double-free or UAF surfaces as
// ran=false (-999); otherwise the exit code, which every program sets to the
// live-object count.
int run(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// A class with a live counter, a throwing block-bodied property, a throwing
// indexer, a throwing method, and non-throwing twins of each.
const char* kSrc =
    "module M; "
    "class Src "
    "  public static _live: int := 0; "
    "  private _v: int := 5; "
    "  public constructor() Src._live := Src._live + 1; end constructor; "
    "  destructor Src._live := Src._live - 1; end destructor; "
    "  public property Boom: int "
    "    get => do throw new InvalidOperationException(\"no\"); end; "
    "  end Boom; "
    "  public virtual property VBoom: int "
    "    get => do throw new InvalidOperationException(\"no\"); end; "
    "  end VBoom; "
    "  public property ABoom: int get => T.Boom(); end ABoom; "
    "  public property N: int get => _v; end N; "
    "  public property [i: int]: int "
    "    get => do throw new InvalidOperationException(\"no\"); end; "
    "  end; "
    "  public method BoomM(): int "
    "    throw new InvalidOperationException(\"no\"); "
    "  end BoomM; "
    "  public method Add(k: int): int return _v + k; end Add; "
    "end Src; "
    "class T "
    "  public static method Boom(): int "
    "    throw new InvalidOperationException(\"no\"); "
    "  end Boom; "
    "end T; ";

std::string main_(const std::string& body) {
    return std::string(kSrc) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

// Twenty iterations that throw out of `expr`, caught in the same frame, then
// `return Src._live`.  0 == every throw-edge receiver reclaimed exactly once.
std::string throwLoop(const std::string& expr) {
    return main_(
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try "
        "      var c := " + expr + "; "
        "      caught := caught + c; "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 20 then return 90; end if; "
        "  return Src._live;");
}

}  // namespace

// ===========================================================================
// FAIL-WITHOUT / PASS-WITH — the four spellings of the family, throw edge.
// Each returned 20 pre-fix (all twenty receivers live at exit); 0 post-fix.
// ===========================================================================

// §23.3 block-bodied getter — the issue's own repro.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           block_property_getter_throw_edge_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(throwLoop("(new Src()).Boom")), 0);
}

// §12.1 / §23: a VIRTUAL property dispatches through the vtable
// (lowerVirtualPropertyGet) — a separate lowering path with the same gap.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           virtual_property_getter_throw_edge_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(throwLoop("(new Src()).VBoom")), 0);
}

// §23.2 expression-bodied getter — the body is INLINED into the caller, so the
// throw comes from a nested call inside it, not from a standalone accessor.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           arrow_property_getter_throw_edge_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(throwLoop("(new Src()).ABoom")), 0);
}

// §23.4 indexer read (#2814's five getter paths).
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           indexer_getter_throw_edge_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(throwLoop("(new Src())[1]")), 0);
}

// §22 method call (#1039/#1053's own site) — the callee throws.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           method_call_throw_edge_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(throwLoop("(new Src()).BoomM()")), 0);
}

// ===========================================================================
// The sentence's own shape: "an operand or ARGUMENT evaluated later throws
// before the temporary's transfer or consumption point".  The receiver is
// produced first, then a later argument / index throws — so the registration
// must sit at the receiver's PRODUCTION, before the argument list, not merely
// around the accessor call.
// ===========================================================================

BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           later_call_argument_throws_receiver_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(throwLoop("(new Src()).Add(T.Boom())")), 0);
}

BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           later_index_argument_throws_receiver_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(throwLoop("(new Src()).Add((new Src())[0])")), 0);
}

// §28.3: the producer need not be a `new` — an `owned`-returning call is the
// other fresh-producer form the same rule governs.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           owned_returning_producer_throw_edge_disposed_spec_28_3) {
    std::string src = std::string(kSrc) +
        "class Mk public static method Make(): owned Src "
        "  return new Src(); end Make; end Mk; "
        "class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try "
        "      var c := Mk.Make().Boom; "
        "      caught := caught + c; "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 20 then return 90; end if; "
        "  return Src._live; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// ORDERING — "the pass runs in reverse order of production on the
// establishment-ordered cleanup stack".  Two temporaries are live at the throw:
// `A` (the call's receiver, produced first) and `B` (the argument's receiver,
// produced second, whose getter throws).  Each destructor folds its own digit
// into `Reg.ord`, so the exit code SPELLS the order: 21 == B then A (reverse of
// production), 12 would be production order.
// ===========================================================================

BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           two_temporaries_disposed_in_reverse_production_order_spec_28_3) {
    std::string src =
        "module M; "
        "class Reg public static ord: int := 0; end Reg; "
        "class A "
        "  public constructor() end constructor; "
        "  destructor Reg.ord := Reg.ord * 10 + 1; end destructor; "
        "  public method Add(k: int): int return k + 1; end Add; "
        "end A; "
        "class B "
        "  public constructor() end constructor; "
        "  destructor Reg.ord := Reg.ord * 10 + 2; end destructor; "
        "  public property Boom: int "
        "    get => do throw new InvalidOperationException(\"no\"); end; "
        "  end Boom; "
        "end B; "
        "class C public static method Main(): int "
        "  var caught := 0; "
        "  try "
        "    var c := (new A()).Add((new B()).Boom); "
        "    caught := caught + c; "
        "  catch var e: InvalidOperationException => "
        "    caught := caught + 1; "
        "  end; "
        "  if caught <> 1 then return 90; end if; "
        "  return Reg.ord; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 21);
}

// Both live temporaries must actually be reclaimed, not merely ordered.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           two_temporaries_both_reclaimed_spec_28_3) {
    std::string src =
        "module M; "
        "class A "
        "  public static _live: int := 0; "
        "  public constructor() A._live := A._live + 1; end constructor; "
        "  destructor A._live := A._live - 1; end destructor; "
        "  public method Add(k: int): int return k + 1; end Add; "
        "end A; "
        "class B "
        "  public static _live: int := 0; "
        "  public constructor() B._live := B._live + 1; end constructor; "
        "  destructor B._live := B._live - 1; end destructor; "
        "  public property Boom: int "
        "    get => do throw new InvalidOperationException(\"no\"); end; "
        "  end Boom; "
        "end B; "
        "class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try "
        "      var c := (new A()).Add((new B()).Boom); "
        "      caught := caught + c; "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 20 then return 90; end if; "
        "  return A._live * 10 + B._live; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// EDGE VARIANTS — where the exception is handled, and what else is on the way
// out.  All four must reclaim the temporary exactly once.
// ===========================================================================

// The exception PROPAGATES out of the producing method to a caller's handler,
// rather than being caught in the same frame.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           propagating_out_of_method_disposed_spec_28_3) {
    std::string src = std::string(kSrc) +
        "class H public static method Go(): int "
        "  var c := (new Src()).Boom; return c; end Go; end H; "
        "class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try caught := caught + H.Go(); "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 20 then return 90; end if; "
        "  return Src._live; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.10 (v0.252): the same shape with a `finally` and NO catch.  The oracle
// here is the LEAK count (`Src._live` must be 0 and the finally must have run 20
// times), not an ordering — the fresh receiver temporary is disposed exactly
// once on the exception edge either way.  The discharge is a runtime null rather
// than an erasure of the registration precisely because the disposal is emitted
// on a block other than the reclaim site, so an erasure at the reclaim would be
// gone by the time that block is emitted.
//
// (Two stale notes corrected here.  The comment used to quote §28.3's "the
// explicit finally runs first … then the implicit auto-dispose runs as the scope
// unwinds further", a sentence the document no longer contains: #2892 ruled
// (v0.241) that a `try`'s own `finally` is established at ENTRY to the protected
// block, so the body scope is now unwind-disposed at the THROW SITE, not deferred
// past the finally.  And the try-with-`finally`-AND-a-matching-catch-arm defect
// it flagged as filed-separately was #2869, fixed at v0.240 and now closed
// structurally by #2892 — see finally_caught_arm_unwind_disposal_2869_test.cpp.)
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           finally_without_catch_disposed_spec_28_3) {
    std::string src = std::string(kSrc) +
        "class Reg public static fin: int := 0; end Reg; "
        "class H public static method Go(): int "
        "  try "
        "    var c := (new Src()).Boom; return c; "
        "  finally Reg.fin := Reg.fin + 1; end; "
        "end Go; end H; "
        "class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try caught := caught + H.Go(); "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 20 then return 90; end if; "
        "  if Reg.fin <> 20 then return 91; end if; "
        "  return Src._live; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §30: an `async` body.  The registration's slot is an entry-block alloca, so
// this pins that a suspend/resume in the same method does not leave a stale or
// garbage pointer behind for the unwind pass.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           async_body_throw_edge_disposed_spec_28_3) {
    std::string src = std::string(kSrc) +
        "class H "
        "  public static async method Step(k: int): Task<int> "
        "    return k + 1; end Step; "
        "  public static async method Go(): Task<int> "
        "    var w := await H.Step(0); "
        "    var c := (new Src()).Boom; "
        "    return c + w; end Go; "
        "end H; "
        "class C public static async method Main(): Task<int> "
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try caught := caught + await H.Go(); "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 20 then return 90; end if; "
        "  return Src._live; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ===========================================================================
// DOUBLE-FREE / UAF DIRECTION — the one "no leak" cannot see, and the only
// direction this fix could plausibly break.
// ===========================================================================

// THE discharge control.  The receiver is reclaimed on the NORMAL path, then
// allocation CHURN recycles the freed block, then a LATER call in the SAME
// scope throws and unwinds through the scope the registration lived in.  If the
// discharge did not null the slot, that unwind pass would dispose an
// already-freed, already-reused block — a heap-use-after-free / double-free
// under generated-code ASan, not a silent no-op.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           discharged_registration_not_refired_after_reclaim_spec_28_3) {
    std::string src = std::string(kSrc) +
        "class Churn "
        "  public static _live: int := 0; "
        "  public a: int := 1; public b: int := 2; "
        "  public constructor() Churn._live := Churn._live + 1; end constructor; "
        "  destructor Churn._live := Churn._live - 1; end destructor; "
        "  public method V(): int return a + b; end V; "
        "end Churn; "
        "class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try "
        "      var a := (new Src()).N; "
        "      caught := caught + a; "
        "      var k := 0; "
        "      while k < 8 do "
        "        using var ch := new Churn(); "
        "        caught := caught + ch.V(); "
        "        k := k + 1; "
        "      end while; "
        "      caught := caught + T.Boom(); "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 600 then return 90; end if; "
        "  return Src._live * 100 + Churn._live; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// A BOUND receiver owns its own disposal; the new registration must not add a
// second one on the throw edge.  `using var s` is disposed at scope exit on
// BOTH edges, so a spurious registration would be a double free.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           bound_receiver_not_disposed_twice_on_throw_edge_spec_28_3) {
    BEZEL_EXPECT_EQ(run(main_(
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try "
        "      using var s := new Src(); "
        "      caught := caught + s.N; "
        "      caught := caught + s.Boom; "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 120 then return 90; end if; "
        "  return Src._live;")), 0);
}

// ACCEPT-CONTROL for the NORMAL path — the edge #2751/#2814 already own.  Mixing
// reclaimed and unwound reads in ONE full expression: the first `.N` is
// reclaimed inline, the `.Boom` unwinds, and the third `.N` never runs.  Every
// produced receiver must be reclaimed exactly once either way.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           normal_and_throw_edges_in_one_expression_spec_28_3) {
    BEZEL_EXPECT_EQ(run(main_(
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try "
        "      var c := (new Src()).N + (new Src()).Boom + (new Src()).N; "
        "      caught := caught + c; "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 20 then return 90; end if; "
        "  return Src._live;")), 0);
}

// ACCEPT-CONTROL: the pure normal path is untouched — no registration survives
// to double-dispose a receiver the inline reclaim already freed.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           normal_path_still_reclaimed_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(main_(
        "  var total := 0; var i := 0; "
        "  while i < 20 do "
        "    total := total + (new Src()).N + (new Src()).Add(1); "
        "    i := i + 1; "
        "  end while; "
        "  if total <> 220 then return 90; end if; "
        "  return Src._live;")), 0);
}

// REVERSED by #4552 (§28.3, v0.306).  This row used to be the EXCLUSION
// control: `freshNewReceiverTempEligible` declined a `string` result on the
// reasoning that it "may alias into the receiver", registration and reclaim
// share that one predicate, so the throw edge declined too and the receiver
// leaked by design — the row asserted `_live == 20`, twenty leaked receivers.
//
// The premise was wrong for a NON-NULLABLE `string`: such a return is minted
// `+1` BY THE CALLEE (the `bz_string_retain` on the return path and its
// expression-body twin, both gated `!nullable`), so `S(k): string return _s;`
// hands back its own reference and disposing the receiver cannot dangle it.
// #4552 therefore reclaims that receiver at §28.3's K8 full-expression
// boundary, with the disposal established AT ITS PRODUCTION — which is what
// makes this throw edge, where the argument `T2.Boom()` throws after the
// receiver exists but before the call, dispose it during unwinding.
//
// So the expectation flips to ZERO live receivers, and the row moves onto the
// `run()` helper's ASan+LSan oracle: the leak it was written to tolerate is
// gone, and an over-release on this edge now faults instead of passing.  A
// `string?` result is still declined (#2196 leaves that path un-refcounted);
// `unbound_owned_receiver_string_result_4552_test.cpp` pins that half.
BEZEL_TEST(fresh_receiver_temp_unwind_2816,
           string_result_receiver_disposed_on_throw_edge_spec_28_3) {
    std::string src =
        "module M; "
        "class Src2 "
        "  public static _live: int := 0; "
        "  private _s: string := \"abc\"; "
        "  public constructor() Src2._live := Src2._live + 1; end constructor; "
        "  destructor Src2._live := Src2._live - 1; end destructor; "
        "  public method S(k: int): string return _s; end S; "
        "end Src2; "
        "class T2 public static method Boom(): int "
        "  throw new InvalidOperationException(\"no\"); end Boom; end T2; "
        "class C public static method Main(): int "
        "  var caught := 0; var i := 0; "
        "  while i < 20 do "
        "    try "
        "      var s := (new Src2()).S(T2.Boom()); "
        "      caught := caught + s.Length; "
        "    catch var e: InvalidOperationException => "
        "      caught := caught + 1; "
        "    end; "
        "    i := i + 1; "
        "  end while; "
        "  if caught <> 20 then return 90; end if; "
        "  return Src2._live; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}
