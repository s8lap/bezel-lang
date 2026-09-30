// Regression tests for issue #2922 (language spec §8.1/§8.3/§8.5, v0.240):
// the §8.1 universal-Object instance surface, called on a receiver this
// compiler has no lowering for, degraded to a ZERO instead of being rejected.
//
// §8.1 declares `Object` with `Equals(other: Object?): bool`,
// `GetHashCode(): int` and `ToString(): string`, and states verbatim: "`Object`
// is the universal root. Every type derives from it". §8.5 fixes the default
// ToString ("For classes: the type's qualified name"). So none of these three
// names is ever an "unknown member" a permissive rule may shrug at — the member
// is guaranteed present on every receiver, and a compiler that cannot lower it
// must say so.
//
// #2457 DOES say so, loudly and precisely — but it was landed as a
// slice-by-slice POSITIVE LIST of receiver names (`Array`/`OwningArray`, then
// `Stack`/`Queue`/…), and every receiver no slice had reached fell through to
// codegen's zero-degrade instead. Measured on `f069645`, across 20 built-in
// receivers × 3 syntactic positions, before this fix:
//
//   `var s := x.ToString(); return s.Length;`   SIGSEGV on 14 receivers — the
//       degraded `string` is null and `.Length` reads absolute address 0x8
//       (ASan: "SEGV on unknown address 0x000000000008 … Hint: address points
//       to the zero page").
//   `var h := x.GetHashCode();`                 silently 0, every instance.
//   `var b := x.Equals(x); if b then … end;`    MALFORMED IR — the degrade
//       emits an `i32` where the language type is `bool`, so the build fails
//       with "Branch condition is not 'i1' type!".
//
// ⚠ ONE missing lowering, THREE different observable outcomes, decided purely
// by WHERE the call is written — and two of the three are loud only by
// accident (the IR verifier, and #1386's interpolated-part reporter, each
// happened to reject the degraded value; nothing checked whether the operation
// was supported). A test that pinned a single syntactic position would pass
// while the hazard survived in the others, so every case below is written
// three ways.
//
// The fix lives at codegen's end-of-chain member-call fallthrough
// (LlvmBackend_Calls2.cpp, the `!ms` block), where an §8.1 member that nothing
// lowered is reported as #2465's established located Error. It is gated on the
// MEMBER — never on a receiver NAME, which is the list-shaped mistake that
// produced this gap and would recreate it for the next built-in added — and on
// the receiver's being a DECLARATION-LESS NAMED built-in, which is the
// population #2922 measured and the only one for which "every arm declined"
// means anything about the member at all. See the false-reject section below
// for the measurement that forced the second half.
// It is also the only sound gate available there: `lowerCallExpr` returns
// nullptr both for "nothing lowered this" and for "an arm lowered it and the
// result is unit" (`tryLowerChannelAndDomainCall`'s `Send` arm emits a real
// `bz_channel_sender_send` and then returns nullptr — #2963), and §8.1 gives these
// three members the return types `bool`/`int`/`string` — none of them unit —
// so for them, and only for them, a null return is unambiguously "not
// lowered". The ANTI-OVER-REJECT controls at the bottom pin that.
//
// Note this is a DIFFERENT cause from #2935 (a member that does not exist at
// all) even though both funnel into the same #2434 zero-degrade: `Mutex`
// REJECTS a bogus member in sema and still degraded on `ToString()`. Both
// receivers are pinned here for that reason.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by exit code or
// `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

const char* kUnimplemented =
    "is declared via the universal Object contract (language spec \xC2\xA7"
    "8.1) but is not yet implemented by this compiler for this receiver";

// Position A — `ToString()` bound to a local, then READ through `.Length`.
// This is the memory-unsafe one: before the fix the local held a null string
// and the `.Length` read faulted at address 0x8.
std::string toStringThenLength(const char* ctor) {
    return std::string(
        "module M; import System.Threading; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var x := ") + ctor + "; "
        "  var s := x.ToString(); "
        "  return s.Length; "
        "end Main; end C;";
}

// Position B — `GetHashCode()` bound to a local and printed. Before the fix
// this was the SILENT one: a constant 0 for every instance, no diagnostic, no
// crash — which would degrade any hash container keyed on such a type to a
// linear scan with nothing to notice.
std::string getHashCodeThenPrint(const char* ctor) {
    return std::string(
        "module M; import System.Threading; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var x := ") + ctor + "; "
        "  var h := x.GetHashCode(); "
        "  Console.WriteLine(h.ToString()); "
        "  return 0; "
        "end Main; end C;";
}

// Position C — `Equals()` bound to a local, then BRANCHED on. Before the fix
// this was loud only by accident: the degraded `i32` reached a `br` that
// demanded an `i1`, so the IR verifier rejected it. Bind it and never branch
// and the same wrongly-typed value shipped.
std::string equalsThenBranch(const char* ctor) {
    return std::string(
        "module M; import System.Threading; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var x := ") + ctor + "; "
        "  var b := x.Equals(x); "
        "  if b then Console.WriteLine(\"eq\"); end; "
        "  return 0; "
        "end Main; end C;";
}

}  // namespace

// ── The three positions, on a receiver sema ACCEPTS the bogus member on ──────
//
// `Random` (stdlib §2.7) is #2922's own repro receiver and #2935's: it has no
// closed-surface sema arm at all, so every position below reached codegen
// untouched.

BEZEL_TEST(object_contract_unlowered_member_2922,
           tostring_then_length_on_random_rejected_spec_8_1_8_5) {
    BEZEL_EXPECT_TRUE(
        hasError(toStringThenLength("new Random()"), kUnimplemented));
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           gethashcode_on_random_rejected_spec_8_1_8_3) {
    BEZEL_EXPECT_TRUE(
        hasError(getHashCodeThenPrint("new Random()"), kUnimplemented));
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           equals_then_branch_on_random_rejected_spec_8_1_8_3) {
    BEZEL_EXPECT_TRUE(
        hasError(equalsThenBranch("new Random()"), kUnimplemented));
}

// ── The same three positions on a receiver sema REJECTS a bogus member on ────
//
// `Mutex` (stdlib §6.2) has a closed-surface sema arm — `m.TotallyBogus()` is
// a clean "no member `TotallyBogus` on `Mutex`" — and STILL degraded on
// `ToString()`, because sema types `ToString` through a separate universal
// rule that never consults that arm. That is the evidence #2922 and #2935 are
// two causes funnelling into one fallback, and why fixing either does not fix
// the other.

BEZEL_TEST(object_contract_unlowered_member_2922,
           tostring_then_length_on_mutex_rejected_spec_8_1_8_5) {
    BEZEL_EXPECT_TRUE(
        hasError(toStringThenLength("new Mutex()"), kUnimplemented));
}

// ── `Stopwatch` is no longer a #2922 rejection example (superseded by #4126) ─
//
// Unlike `Random`/`Mutex` above, `Stopwatch` (stdlib §8.1) has a PUBLIC
// CONSTRUCTOR (`public constructor();`), so `new Stopwatch()` types to a
// declaration-less Named receiver distinct from `Stopwatch.StartNew()`'s.
// `narrow #4126` (stdlib §8.1/§6.1/§2.6, v0.295) gave exactly that receiver
// shape a real, correct dispatch for all three universal-Object members —
// a qualified-name `ToString()` (the same `kBuiltinClassModulePaths` literal
// `array_tostring_still_returns_qualified_name_spec_8_5` below already pins
// for `Array`/`Stack`) and a reference-identity `Equals()`/`GetHashCode()`
// (§8.3's class default). The #2922 "not yet implemented" rejection these
// two rows used to pin is therefore now WRONG — the member is implemented —
// so §7 of this project's lane discipline requires updating the pin rather
// than treating the broken rows as a regression.
//
// Independently re-verified (fix-forward for #4126, 2026-09-10) under ASan
// across the full `new Stopwatch()` surface (construct, Start/Stop/
// IsRunning/Elapsed accessors, ToString, Equals, GetHashCode, and the same
// via `StartNew()`): no crash, no leak, correct values on every member.
// Kept in THIS file (rather than deleted) rather than moved, because the
// shape — a built-in with a public constructor, reached through the
// universal-Object gate — is exactly the population #2922/#4126 are both
// about, and a reader tracing either issue should find the current, not the
// superseded, behavior here.

BEZEL_TEST(object_contract_unlowered_member_2922,
           tostring_then_length_on_stopwatch_dispatches_qualified_name_spec_8_1_8_5) {
    // #4126 (stdlib §8.1, v0.295): "System.Diagnostics.Stopwatch" (28 chars)
    // — the qualified-name default, not a rejection.
    auto r = runAot(toStringThenLength("new Stopwatch()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(28, r.exitCode);
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           equals_then_branch_on_stopwatch_reference_identity_spec_8_1_8_3) {
    // #4126 (stdlib §8.1/§8.3, v0.295): reference-identity Equals — a
    // Stopwatch compared to itself is equal and the branch is taken, the
    // same shape as array_equals_reflexive_still_true_spec_8_3 below.
    auto r = runAot(equalsThenBranch("new Stopwatch()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("eq") != std::string::npos);
}

// A generic-typed receiver reaches the same fallthrough through a different
// door: `Buffer<T>` (stdlib §4.14 family) carries generic args, so this pins
// that the gate does not depend on the receiver being nullary.
BEZEL_TEST(object_contract_unlowered_member_2922,
           tostring_then_length_on_generic_builtin_rejected_spec_8_1) {
    BEZEL_EXPECT_TRUE(
        hasError(toStringThenLength("new Buffer<int>(4)"), kUnimplemented));
}

// ── The FALSE REJECT this gate caused, and the narrowing that fixed it ──────
//
// Gate 2 bisected `sema_this_indexer_typing_2795.this_index_substitutes_against
// _the_enclosing_instantiation_14_1` to this fix. The gate was telling a program
// that `int.ToString()` "is not yet implemented" — false, and unlocated (0:0).
//
// CHARACTERISED BEFORE NARROWING, on a 2x2x6 grid (enclosing class generic or
// not x expression or block body x six receiver expressions). It is the
// COMPOSITION of all three that fails; every single-axis relaxation compiled:
// a non-generic class, a block body, or any receiver other than `this[...]`
// (a property, a parameter, a method call, a literal) was fine.
//
// MECHANISM. `this[i]` does not lower in that shape, and every §8.1 dispatch
// path asks for the RECEIVER'S value first — CG27c
// (LlvmBackend_Exprs.cpp, `recv = lowerExpr(*ma->target); if (recv) return
// emitPrimitiveToString(...)`) for a primitive, the `isNamedClassRecv` arm for
// a class/interface/Object. Both fall through when the receiver is null, and
// the gate then blamed the MEMBER. That is this fix's own #2963 hazard one
// level up: inferring "no lowering exists for M on R" from "this call site did
// not lower HERE".
//
// THE NARROWING: the gate now fires only for a DECLARATION-LESS NAMED receiver
// — a built-in with no TypeSymbol and no vtable, so no thunk for
// `lowerObjectToString` and no `emitPrimitiveToString`, which is exactly why
// #2457 exists and exactly the population #2922 measured. The 20x4 receiver
// matrix is byte-identical before and after, so the narrowing costs no true
// positive.

BEZEL_TEST(object_contract_unlowered_member_2922,
           primitive_receiver_in_generic_expression_body_not_rejected_spec_14_1) {
    // The bisected repro. `int.ToString()` is implemented for every primitive;
    // the gate must not claim otherwise however the receiver is spelled.
    const std::string program =
        "module M; "
        "class Holder<T> "
        "  public property [i: int]: int => i + 5; "
        "  public method N(i: int): int => this[i].ToString().Length; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<string>(); "
        "  return h.N(2); "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(1, r.exitCode);   // (2+5).ToString() == "7", .Length == 1
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           string_receiver_in_generic_expression_body_not_rejected_spec_14_1) {
    const std::string program =
        "module M; "
        "class Holder<T> "
        "  public property [i: int]: string => \"abc\"; "
        "  public method N(i: int): int => this[i].ToString().Length; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder<string>(); "
        "  return h.N(2); "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           user_class_receiver_in_generic_expression_body_not_rejected_spec_14_1) {
    // The NAMED half, and the reason restricting by receiver KIND alone was not
    // enough: a plain user class with a working §8.5 default reached the gate
    // through the identical shape. Only "declaration-less" excludes it.
    const std::string program =
        "module M; "
        "class R public method Nop(): int => 0; end R; "
        "class H<T> "
        "  private owned _r: R; "
        "  public constructor() _r := new R(); end constructor; "
        "  public property [i: int]: R => this._r; "
        "  public method N(i: int): int => this[i].ToString().Length; "
        "end H; "
        "class C public static method Main(): int "
        "  using var h := new H<string>(); "
        "  return h.N(2); "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    // #2934 (§8.5, v0.252): the §8.5 class default is module-path-qualified,
    // so it is "M.R" (3) — was "R" (1).
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           the_diagnostic_is_located_when_it_does_fire_2874) {
    // The false reject also printed `0:0` — a body re-lowered from a
    // RECONSTRUCTED AST carries no spans. Even where the gate is RIGHT, an
    // unlocated diagnostic is a poor one (#2874 fixed exactly this shape for
    // its own message), so the report now falls back through the callee and
    // then the receiver. Pinned by asserting the driver's `path:line:col:`
    // prefix is not `0:0`.
    auto r = runAot(toStringThenLength("new Random()"));
    bool sawLocated = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(kUnimplemented) == std::string::npos) continue;
        BEZEL_EXPECT_TRUE(d.message.find(":0:0:") == std::string::npos);
        sawLocated = true;
    }
    BEZEL_EXPECT_TRUE(sawLocated);
}

// ── ANTI-OVER-REJECT controls ───────────────────────────────────────────────
//
// The gate must fire ONLY where nothing lowered. These pin the implemented
// side of the same three members and must keep their exact VALUES.

BEZEL_TEST(object_contract_unlowered_member_2922,
           array_tostring_still_returns_qualified_name_spec_8_5) {
    // §8.5: "For classes: the type's qualified name" — fix #3119 (stdlib
    // §2.13 rule 1 + rule 3): the literal is now the QUALIFIED module path
    // with the rule 3 generic-args tail —
    // "System.Collections.Generic.Array<int>", length 37 (was 5, "Array").
    auto r = runAot(toStringThenLength("new Array<int>(4, 0)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(37, r.exitCode);   // #3119: was 5
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           stack_tostring_still_returns_qualified_name_spec_8_5) {
    // fix #3119: Stack<int> renders as
    // "System.Collections.Generic.Stack<int>", length 37 (was 5, "Stack").
    auto r = runAot(toStringThenLength("new Stack<int>()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(37, r.exitCode);   // #3119: was 5
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           array_equals_reflexive_still_true_spec_8_3) {
    // §8.3, "Class | Reference equality" — an Array compared to itself is
    // equal, and the branch must still be taken.
    auto r = runAot(equalsThenBranch("new Array<int>(4, 0)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("eq") != std::string::npos);
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           user_class_object_members_unaffected_spec_8_3_8_5) {
    // The control #2922's inventory used: an ordinary user class, all three
    // members, in the same three positions, in one program. §8.5's class
    // default is the qualified name ("P" — 1 character); §8.3's class default
    // is reference equality (reflexively true) and an identity hash. All three
    // lower, so none may be rejected.
    //
    // `h` is deliberately consumed by a COMPARISON rather than by
    // `h.ToString()`: `GetHashCode()`'s result carries no sema type on any
    // class receiver, so `h.ToString()` reaches the very fallthrough this fix
    // reports from. That is a real, separate defect (measured for this fix: it
    // silently DROPPED the whole `Console.WriteLine`), filed as #2964; this
    // control must pin the members, not that defect.
    const std::string program =
        "module M; "
        "class P public method Nop(): int return 0; end Nop; end P; "
        "class C public static method Main(): int "
        "  using var p := new P(); "
        "  var s := p.ToString(); "
        "  var b := p.Equals(p); "
        "  var h := p.GetHashCode(); "
        "  if b then Console.WriteLine(\"eq\"); end; "
        "  if h <> 0 then Console.WriteLine(\"hash\"); end; "
        "  Console.WriteLine(s); "
        "  return s.Length; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    // #2934 (§8.5, v0.252): "M.P" (3) — was "P" (1).
    BEZEL_EXPECT_EQ(3, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("eq") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("hash") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("P") != std::string::npos);
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           unconstrained_generic_object_members_still_accepted_spec_14_2) {
    // §14.2/#1078's ANTI-OVER-REJECT control, byte-for-byte the shape of
    // tests/lit/sema/method_constraint_callsite_746__object_members_on_
    // unconstrained_t_accepted_1078.bzl — the ONE §3/#605-permissive program
    // in the corpus that names an §8.1 member. It reaches the same
    // fallthrough, in the DEAD generic template body (the method is never
    // instantiated), where no substitution exists and nothing is ever called.
    // `emittingGenericTemplateBody_` excludes it; §14.2's promise is about the
    // LIVE specializations, where `T` is concrete and the ordinary
    // primitive/class/vtable paths lower the call long before this point.
    const std::string program =
        "module M; "
        "static class U public static method CheckObj<T>(x: T): int "
        "  var s := x.ToString(); var h := x.GetHashCode(); return h; "
        "end CheckObj; end U; "
        "class C public static method Main(): int return 0; end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           live_generic_specialization_object_members_work_spec_14_2) {
    // The other half of the same promise, which the dead-template exclusion
    // must not have cost: the SAME generic method actually instantiated, at a
    // primitive and at a user class. Both must lower and produce real values.
    const std::string program =
        "module M; "
        "class P public method Nop(): int return 0; end Nop; end P; "
        "static class U public static method Describe<T>(x: T): string "
        "  return x.ToString(); "
        "end Describe; end U; "
        "class C public static method Main(): int "
        "  var a := U.Describe<int>(7); "
        "  using var p := new P(); "
        "  var b := U.Describe<P>(p); "
        "  Console.WriteLine(a); "
        "  Console.WriteLine(b); "
        "  return a.Length + b.Length; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    // "7" (1) + "M.P" (3) — the §8.5 primitive representation and the
    // class default, module-path-qualified since #2934 (§8.5, v0.252);
    // was 2.
    BEZEL_EXPECT_EQ(4, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("7") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("P") != std::string::npos);
}

BEZEL_TEST(object_contract_unlowered_member_2922,
           unit_returning_builtin_call_still_lowers_spec_30_15) {
    // THE false-reject this fix's first draft actually caused, kept as a
    // permanent control. `ChannelSender<T>.Send` (stdlib §6.9 / spec §30.15)
    // is fully implemented — `tryLowerChannelAndDomainCall` emits
    // `bz_channel_sender_send` — and then returns nullptr because the result
    // is unit, so execution reaches the very same fallthrough this fix reports
    // from. A receiver-keyed gate there rejected this program (and four other
    // green lit programs); the §8.1 member gate cannot, because none of
    // `ToString`/`Equals`/`GetHashCode` is unit-returning. The oracle is the
    // channel's buffered `Count` (stdlib §6.9: "items currently buffered")
    // AFTER the send — exit 0 alone would also hold if the send were dropped.
    const std::string program =
        "module M; import System.Threading; "
        "class C public static method Main(): int "
        "  using var ch := new Channel<int>(); "
        "  using var sender := ch.NewSender(); "
        "  sender.Send(42); "
        "  sender.Send(7); "
        "  return ch.Count; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(2, r.exitCode);
}
