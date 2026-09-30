// Spec-derived regression tests for issue #3751 — a `this(...)` / `base(...)`
// CONSTRUCTOR CHAIN CALL was classified against NO formal parameter list, so
// the chained-to constructor's `owned` parameters were invisible to §22.3's
// call-site-marker rule and to §28.8's ownership transfer.
//
// ── THE SHAPE (issue 3751's repro, verbatim) ───────────────────────────────
//
//     class Base(owned r: Res)
//       private owned _s: Res := r;
//       public constructor(owned x: Res)
//         this(x);
//       end constructor;
//       public method Id(): int  return _s.Id();  end Id;
//     end Base;
//
//     using var b := new Base(new Res(7));   -> heap-use-after-free in Base.Id
//
// ── WHAT THE DEFECT ACTUALLY WAS (measured on r12-cycle-1 @ a31fb7b) ───────
//
// `ownership::resolveCalleeParams` answers "what are this callee's formals?"
// off `SemaContext::exprSymbol(callee)`. A chain call's callee is the BARE
// IDENTIFIER `this` or `base` (`ast::chainCallOfStmt`), which resolves to no
// `ConstructorSymbol` at all — the chain target is selected by ARITY against
// the enclosing class (or its base), which is what codegen's `emitChainCall`
// does. So every chain call answered "no formals", and with no formals:
//
//   * `isImplicitOwned` said the argument position was not an owned sink, so
//     §28.8's transfer never ran: the `owned` parameter `x` was never marked
//     moved-from, stayed Owned to the end of the convenience constructor, and
//     the §28.3 auto-drop FREED it — while the chained-to constructor had
//     already transferred the same object into the `owned` field `_s`. The next
//     `_s` read is a use-after-free. Only a WARNING was emitted.
//   * §28.8's borrow-into-owned-formal reject (`checkOwnedArgSource`) never ran
//     either, so a plain BORROW parameter chained into an `owned` slot was
//     accepted and double-owned (row T8).
//   * Conversely `this(owned x)` was REJECTED with "this parameter position
//     does not take ownership" — naming a position that IS `owned`. So no
//     correct spelling of the transfer existed.
//
// ── THE BRIEF WAS NARROWER THAN THE DEFECT ─────────────────────────────────
//
// Issue 3751 attributes the hole to "a §22.10 PRIMARY constructor has no
// ConstructorSymbol". Measured on a31fb7b, that is only one of three arms; the
// defect is in the CHAIN CALLEE, not in the primary:
//
//   this(x) -> §22.10 primary `class Base(owned r: Res)`      UAF (row T3)
//   this(x) -> explicit SIBLING `constructor(owned r: Res)`   UAF (row T5)
//   base(x) -> base class's    `constructor(owned r: Res)`    UAF (row T7)
//
// All three are one predicate and land together (CLAUDE.md §6b: a family lands
// whole or not at all). The fix routes both owned-sink classifiers through
// `MethodOwnership::calleeOrChainFormals`, whose chain arm calls the SAME
// `resolveCtorFormals` a `new T(args)` call site calls — there is no
// chain-specific copy of the classification.
//
// ── WHICH SPELLING IS CORRECT ──────────────────────────────────────────────
//
// §22.3 "Parameter modifiers", v0.286: "when the argument is a **named owned
// binding** — an owned local or an `owned` parameter, which the call leaves
// moved-from — the caller writes the transfer marker at the call site". The
// compiler admits both spellings of that marker (the `owned` argument modifier
// of v0.169 and the `move` expression §22.3 names today), and both are pinned
// here — T1 and T2 — because a fix that admitted only one would leave the other
// silently rejected at a chain while it works at every ordinary call.
//
// §12.8 "Constructor chaining" puts the chain call in the leading statement
// position, which is why the grid below has no "owned LOCAL chained" row: a
// `var t := new Res(n); this(t);` is rejected by §12.8 before ownership sees
// it. The only named owned binding a chain argument can be is the convenience
// constructor's own `owned` parameter.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `Res` carries BOTH static counters. `made` alone cannot separate "constructed
// once and disposed twice" from "constructed twice"; `live` alone cannot see a
// value that was never made. The pair is the oracle.
const char* kRes =
    "class Res "
    "  public static _live: int := 0; "
    "  public static _made: int := 0; "
    "  private _id: int; "
    "  public constructor(id: int) "
    "    _id := id; "
    "    _live := _live + 1; "
    "    _made := _made + 1; "
    "  end constructor; "
    "  destructor "
    "    _live := _live - 1; "
    "  end destructor; "
    "  public method Id(): int => _id; "
    "end Res; ";

// The §28.3 (v0.169 / §22.3) missing-owned-marker diagnostic —
// `reportMissingOwnedMarker`. Matched on TEXT plus its section citation, never
// on `hasErrors()`.
bool hasMissingOwnedMarker(const OwnershipResult& r, const std::string& name) {
    const std::string needle = "this call consumes `" + name + "`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos &&
            d.message.find("§22.3") != std::string::npos)
            return true;
    return false;
}

// §28.8 (#395) borrow-into-`owned`-formal reject — `checkOwnedArgSource`.
// The diagnostic names the RULE, not the binding ("a borrow cannot be passed
// where an `owned` value is required: ... (§28.8 — transfer an owned value with
// `move`, `new`, or a call returning `owned T`)"), so it is matched on that text
// plus its section citation. Matching on the binding name was this helper's
// first spelling and was vacuously false — caught by the row failing, which is
// the point of asserting TEXT rather than `hasErrors()`.
bool hasBorrowIntoOwnedReject(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("a borrow cannot be passed where an `owned` value "
                           "is required") != std::string::npos &&
            d.message.find("§28.8") != std::string::npos)
            return true;
    return false;
}

// The §28.3 auto-drop WARNING on an `owned` parameter. Its presence is the
// pre-fix signature: it fires exactly when the parameter was still Owned at the
// end of the convenience constructor — i.e. when the chain did NOT consume it,
// which is when codegen emits the free that the chained-to constructor's field
// store makes a use-after-free. Its ABSENCE on an accepted row is therefore
// part of the fix's proof, not cosmetic.
bool hasOwnedParamAutoDropWarning(const OwnershipResult& r,
                                  const std::string& name) {
    const std::string needle = "owned parameter `" + name + "`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos &&
            d.message.find("auto-drop") != std::string::npos)
            return true;
    return false;
}

// A §22.10 primary constructor whose sole parameter is `owned` and is consumed
// by an explicit `owned` field initializer.
const char* kPrimaryOwned =
    "class Base(owned r: Res) "
    "  private owned _s: Res := r; "
    "  public method Id(): int "
    "    return _s.Id(); "
    "  end Id; "
    "end Base; ";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// T1 — ACCEPT, ASan/LSan, 200 iterations. `this(owned x)` into an `owned`
// §22.10 PRIMARY-constructor parameter. This is the spelling issue 3751 says
// had NO correct form: pre-fix it was rejected outright.
// FAIL-WITHOUT: compile error "the `owned` call-site marker is only valid on an
// argument to an `owned` parameter" (measured on a31fb7b).
// The convenience constructor carries a trailing `salt: int` so its parameter
// list differs from the primary's `(owned r: Res)`: #5132 (§9.3 / §11.3 /
// §22.6, v0.313) rejects a body constructor with the primary's parameter list
// (`(owned x: Res)` differs from it only by name), and #5099 had already made
// that shape a stage-8 tie. `new Base(new Res(i), 0)` selects the convenience
// constructor by arity; the chain-ownership behavior under test is unchanged.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 (convenience constructors chain via `this(...)`) / §22.3 (call-site
// marker) / §28.8 (ownership transfer), issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           convenience_ctor_owned_marker_to_owned_primary_ctor_param_asan_x200) {
    const std::string src =
        std::string("module M3751a; ") + kRes +
        "class Base(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public constructor(owned x: Res, salt: int) "
        "    this(owned x); "
        "  end constructor; "
        "  public method Id(): int "
        "    return _s.Id(); "
        "  end Id; "
        "end Base; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    var acc: int := 0; "
        "    while i < 200 do "
        "      using var b := new Base(new Res(i), 0); "
        "      acc := acc + b.Id(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"acc={acc} made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    // `ran == false` is BOTH the ASan/LSan failure signal and the "cannot build
    // an instrumented executable here" skip — assert it so neither passes
    // silently.
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // `made=200` is exactly-once construction; `live=0` is exactly-once
    // disposal. `acc` is the sum 0..199 and proves the TRANSFERRED value — not
    // a zeroed slot — reached the field.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=19900 made=200 live=0") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T2 — ACCEPT, ASan/LSan, 200 iterations. The same chain written with the
// §22.3 `move` marker. The `move` spelling happened to WORK pre-fix — but only
// because `recordMovesIn` marks a `move` operand moved-from without consulting
// any formal list at all. It was therefore accepted for a PLAIN parameter too;
// T11 pins that the fix does not lose the correct rejection.
// Same `salt: int` parameter-list distinction from the primary as T1 (#5132).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 (the transfer marker `M(move x)`) / §28.8.1, issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           convenience_ctor_move_marker_to_owned_primary_ctor_param_asan_x200) {
    const std::string src =
        std::string("module M3751b; ") + kRes +
        "class Base(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public constructor(owned x: Res, salt: int) "
        "    this(move x); "
        "  end constructor; "
        "  public method Id(): int "
        "    return _s.Id(); "
        "  end Id; "
        "end Base; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    var acc: int := 0; "
        "    while i < 200 do "
        "      using var b := new Base(new Res(i), 0); "
        "      acc := acc + b.Id(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"acc={acc} made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=19900 made=200 live=0") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T3 — REJECT. THE HEADLINE REPRO: bare `this(x)` where `x` is the convenience
// constructor's own `owned` parameter and the §22.10 primary's parameter is
// `owned`. §22.3: "Omitting the marker on a named owned binding is a compile
// error naming the fix".
// FAIL-WITHOUT: accepted with only the auto-drop WARNING, and a
// `heap-use-after-free` at run time (measured on a31fb7b).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 (call-site marker required on a named owned binding) / §22.10,
// issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           convenience_ctor_unmarked_owned_param_to_primary_ctor_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; public method Id(): int => 1; "
        "end Res; "
        "class Base(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public constructor(owned x: Res) "
        "    this(x); "
        "  end constructor; "
        "  public method Id(): int "
        "    return _s.Id(); "
        "  end Id; "
        "end Base; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
    // The pre-fix signature must be GONE: with the chain consuming `x`, the
    // parameter is moved-from at the constructor's end, so nothing auto-drops
    // and codegen emits no free.
    BEZEL_EXPECT_FALSE(hasOwnedParamAutoDropWarning(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// T4 — ACCEPT, ASan/LSan, 200 iterations. `this(owned x)` chaining to an
// explicit SIBLING constructor with an `owned` parameter — NOT a primary
// constructor. Issue 3751 attributes the hole to the primary's missing
// `ConstructorSymbol`; this row is the evidence that the defect was in the
// chain CALLEE, and that this arm now works too.
// ═══════════════════════════════════════════════════════════════════════════
// per §12.8 (`this(...)` chains to another constructor in the same class) /
// §22.3 / §28.8, issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           convenience_ctor_chain_to_sibling_owned_ctor_asan_x200) {
    const std::string src =
        std::string("module M3751d; ") + kRes +
        "class Base "
        "  private owned _s: Res; "
        "  public constructor(owned r: Res) "
        "    _s := r; "
        "  end constructor; "
        "  public constructor(owned x: Res, tag: bool) "
        "    this(owned x); "
        "  end constructor; "
        "  public method Id(): int "
        "    return _s.Id(); "
        "  end Id; "
        "end Base; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    var acc: int := 0; "
        "    while i < 200 do "
        "      using var b := new Base(new Res(i), true); "
        "      acc := acc + b.Id(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"acc={acc} made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=19900 made=200 live=0") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T5 — REJECT. The sibling-target twin of T3. Pre-fix this compiled with the
// auto-drop warning and was a `heap-use-after-free` (measured on a31fb7b).
// ═══════════════════════════════════════════════════════════════════════════
// per §12.8 / §22.3, issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           convenience_ctor_unmarked_owned_param_to_sibling_ctor_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; public method Id(): int => 1; "
        "end Res; "
        "class Base "
        "  private owned _s: Res; "
        "  public constructor(owned r: Res) "
        "    _s := move r; "
        "  end constructor; "
        "  public constructor(owned x: Res, tag: bool) "
        "    this(x); "
        "  end constructor; "
        "end Base; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
    BEZEL_EXPECT_FALSE(hasOwnedParamAutoDropWarning(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// T6 — ACCEPT, ASan/LSan, 200 iterations. `base(owned x)` into a BASE class's
// `owned` constructor parameter — §12.8's other chain form, same predicate.
// ═══════════════════════════════════════════════════════════════════════════
// per §12.8 ("A derived class's constructor may chain to a base constructor") /
// §22.3 / §28.8, issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           derived_ctor_base_chain_owned_marker_asan_x200) {
    const std::string src =
        std::string("module M3751f; ") + kRes +
        "class P "
        "  private owned _s: Res; "
        "  public constructor(owned r: Res) "
        "    _s := r; "
        "  end constructor; "
        "  public method Id(): int => _s.Id(); "
        "end P; "
        "class D: P "
        "  public constructor(owned x: Res) "
        "    base(owned x); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    var acc: int := 0; "
        "    while i < 200 do "
        "      using var d := new D(new Res(i)); "
        "      acc := acc + d.Id(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"acc={acc} made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=19900 made=200 live=0") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T7 — REJECT. Bare `base(x)` with `x` the derived constructor's `owned`
// parameter. Pre-fix: warning + `heap-use-after-free` (measured on a31fb7b).
// ═══════════════════════════════════════════════════════════════════════════
// per §12.8 / §22.3, issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           derived_ctor_unmarked_owned_param_to_base_ctor_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; public method Id(): int => 1; "
        "end Res; "
        "class P "
        "  private owned _s: Res; "
        "  public constructor(owned r: Res) "
        "    _s := move r; "
        "  end constructor; "
        "end P; "
        "class D: P "
        "  public constructor(owned x: Res) "
        "    base(x); "
        "  end constructor; "
        "end D; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
    BEZEL_EXPECT_FALSE(hasOwnedParamAutoDropWarning(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// T8 — REJECT. A BORROW parameter chained into an `owned` primary-ctor slot.
// §28.8: the argument's real owner disposes it AND the constructed object's
// generated `Dispose()` disposes the `owned` field — pre-fix this compiled with
// NO diagnostic at all and was a `heap-use-after-free` (measured on a31fb7b:
// `constructor(b: Res) this(b);` printed `id=3` and then faulted). This is the
// second face of the same missing formals lookup.
// ═══════════════════════════════════════════════════════════════════════════
// per §28.8 (a borrow flowing into an `owned` formal) / §22.10, issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           borrow_param_chained_into_owned_primary_ctor_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; public method Id(): int => 1; "
        "end Res; "
        "class Base2(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public constructor(b: Res) "
        "    this(b); "
        "  end constructor; "
        "  public method Id(): int "
        "    return _s.Id(); "
        "  end Id; "
        "end Base2; ");
    BEZEL_EXPECT_TRUE(hasBorrowIntoOwnedReject(r));
}

// ═══════════════════════════════════════════════════════════════════════════
// T9 — ANTI-OVER-REJECT, ASan/LSan, 200 iterations. A FRESH PRODUCER chained
// unmarked into an `owned` primary-ctor parameter. §22.3: "A **fresh
// producer** ... stays unmarked: no live binding is invalidated". This shape
// WORKED before the fix and must keep working — a fix that made every chain
// argument an owned sink requiring a marker would break it.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 (marker economy: a fresh producer stays unmarked) / §22.10,
// issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           fresh_producer_chained_unmarked_to_owned_primary_ctor_param_asan_x200) {
    const std::string src =
        std::string("module M3751i; ") + kRes + kPrimaryOwned +
        "class Conv(owned r2: Res) "
        "  private owned _t: Res := r2; "
        "  public constructor(n: int) "
        "    this(new Res(n)); "
        "  end constructor; "
        "  public method Id(): int "
        "    return _t.Id(); "
        "  end Id; "
        "end Conv; "
        "class C "
        "  public static method Main(): int "
        "    var i: int := 0; "
        "    var acc: int := 0; "
        "    while i < 200 do "
        "      using var c := new Conv(i); "
        "      acc := acc + c.Id(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"acc={acc} made={Res._made} live={Res._live}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("acc=19900 made=200 live=0") !=
                      std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T10 — ANTI-OVER-REJECT, value-verified. A chain into a PLAIN (non-`owned`)
// primary-ctor parameter is untouched: no marker required, no consume, the
// object still builds correctly. This is the row that would fail if the chain
// formals resolution had picked the WRONG target (for instance the enclosing
// convenience constructor itself, whose arity also matches — §22.10's
// no-self-chain exclusion is what prevents that).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 (primary constructors) / §22.8 step 0, issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           plain_primary_ctor_param_chain_unaffected_22_10) {
    const std::string src =
        "module M3751j; "
        "class PBox(n: int, s: string) "
        "  private _n: int := n; "
        "  private _s: string := s; "
        "  public constructor(n2: int) "
        "    this(n2 * 3, \"c\"); "
        "  end constructor; "
        "  public method Show(): string => $\"{_n}/{_s}\"; "
        "end PBox; "
        "class C "
        "  public static method Main(): int "
        "    var b := new PBox(7); "
        "    Console.WriteLine(b.Show()); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("21/c") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T11 — REJECT PRESERVED. The `owned` call-site marker written at a chain whose
// target parameter is PLAIN stays rejected: the spurious-marker rule is what
// keeps the fix from being a blanket "accept `owned` at any chain". Pre-fix
// this was rejected too — but for the wrong reason (no formals at all), so the
// rejection was indistinguishable from the WRONG rejection of `this(owned x)`
// against an `owned` slot. After the fix the two are decided by the target's
// actual modifier.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.3 ("writing it ... on a non-`owned` parameter position is likewise an
// error"), issue #3751
BEZEL_TEST(this_chain_owned_ctor_param_3751,
           owned_marker_at_chain_to_plain_primary_ctor_param_still_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res destructor end destructor; end Res; "
        "class PB(n: Res) "
        "  public constructor(owned x: Res, tag: bool) "
        "    this(owned x); "
        "  end constructor; "
        "end PB; ");
    bool spurious = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("this parameter position does not take ownership") !=
            std::string::npos)
            spurious = true;
    BEZEL_EXPECT_TRUE(spurious);
}
