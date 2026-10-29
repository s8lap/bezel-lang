// Regression tests for issue #2966 (codegen; stdlib closed surfaces, language
// spec §3/#605 permissive typing, §21.7, §22.2 properties, v0.240): #2935's
// residual positions.
//
// #2935 rejects a nonexistent member on a built-in receiver at the two positions
// where a value is DEMANDED — a local declaration and `return expr;` (§21.7).
// It had to buy soundness that way, and its own comment says why: `lowerCall`
// returned nullptr both for "nothing lowered this" and for "an arm lowered it
// and the result is unit", so only a value demand could tell the two apart from
// outside. That left FOUR positions uncovered, each the same typo compiling to a
// plausible 0:
//
//   (a) a BARE STATEMENT — no value-demanding consumer at all, and the quietest
//       form of the bug: not even #2434's warning fires, because there is no
//       local slot to degrade. Measured on `5feaae2`:
//           using var r := new Random();
//           r.TotallyBogusMember(1, 2);   -> exit 0, ZERO diagnostics
//       This is also the one most likely to be hit in practice, since a
//       discarded call is exactly how a side-effecting method is written.
//   (b),(c) the two ARROW-BODY (`=> expr`) faces of the #2491 note;
//   (d) the PROPERTY-GETTER face of it.
//
// #2963 is what makes covering them safe, and it covers all four AT ONCE rather
// than one consumer at a time: the end-of-chain fallthrough in `lowerCall` can
// now ask whether ANY arm emitted side-effecting IR since entry. Nothing
// emitted ⇒ nothing lowered this ⇒ a located Error at the CALL, whatever the
// position. `sender.Send(42)` and `scope.Spawn(…)` — the shapes a value-blind
// version of this gate false-rejected — emit `bz_channel_sender_send` /
// `bz_domain_spawn` and are excluded by construction rather than by a list.
//
// ⚠ #2966's own "suggested fix" was a `ci.returnType != nullptr` test bolted
// onto sites (b) and (c), plus a third unconditional copy at (d). That would
// have made the #2491 diagnostic a FIFTH hand-maintained mirror of itself —
// #2924's standing finding about this exact message — and would still have left
// (a) uncovered, since a bare statement has no consumer to interrogate. One gate
// at the call site is the whole of it.
//
// WHAT #2935's SITES STILL OWN, and why they are not now dead: the half where an
// arm DID emit code and simply produced no value. `var x := sender.Send(42);` is
// real, implemented, valueless, and illegal in a value-demanding position; this
// gate cannot see it and must not, because that is precisely the distinction
// #2963 exists to draw. That shape is pinned in
// tests/codegen/builtin_receiver_unlowered_member_2935_test.cpp.

#include <string>

#include "support/aot_support.hpp"  // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by exit code or
// `!r.ran` alone.
bool hasError(const std::string& program, const std::string& keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(keyword) != std::string::npos)
            return true;
    return false;
}

// The stable part of the shared diagnostic. The opening clause is #2935's
// family wording verbatim; the clause after it is what this site can say that
// #2935's could not — that NOTHING was emitted, which excludes the
// "documented but yields nothing here" reading those sites had to leave open.
//
// #7661 (stdlib §2.5-§2.7, §3.1, v0.350): an undeclared member on `Random`
// is now refused by SEMA ("no member `X` on built-in type `Random`") before
// it reaches this codegen gate, so the per-position rows below use
// `Stopwatch` (stdlib §8.1), a built-in whose undeclared members still reach
// it. The `Random` spelling is pinned at the sema site in
// tests/codegen/builtin_member_degrade_7661_test.cpp.
const std::string kMsg =
    "on built-in type `Stopwatch` produced no value, and no dispatch arm "
    "emitted any code for this call";

}  // namespace

// ── (a) the bare statement — the filed repro, and the quietest form ─────────

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           bogus_member_in_a_bare_statement_is_rejected_stdlib_2_7) {
    const std::string program =
        "module M; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var r := new Stopwatch(); "
        "  r.TotallyBogusMember(1, 2); "
        "  Console.WriteLine(\"compiled\"); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "`TotallyBogusMember` " + kMsg));
    // And the binary that used to ship must not: the pre-fix program compiled,
    // ran, and printed "compiled" with no diagnostic whatsoever.
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("compiled") == std::string::npos);
}

// ── (b)/(c) the two arrow-body faces ────────────────────────────────────────

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           bogus_member_in_a_unit_arrow_body_is_rejected_spec_21_7) {
    // The position #2966's own suggested fix could not reach with a
    // `ci.returnType` test: the callable declares NO return type, so nothing
    // demands a value here — and the typo is still a typo.
    const std::string program =
        "module M; import System.Diagnostics; "
        "class C "
        "  public static method Poke(r: Stopwatch) => r.TotallyBogusMember(1, 2); "
        "  public static method Main(): int "
        "    using var rr := new Stopwatch(); "
        "    C.Poke(rr); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "`TotallyBogusMember` " + kMsg));
}

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           bogus_member_in_a_value_arrow_body_is_rejected_spec_21_7) {
    // The same face with a DECLARED return type — the one #2966 proposed
    // gating on. It must reject too, and through the same one gate.
    const std::string program =
        "module M; import System.Diagnostics; "
        "class C "
        "  public static method Roll(r: Stopwatch): int "
        "      => r.TotallyBogusMember(1, 2); "
        "  public static method Main(): int "
        "    using var rr := new Stopwatch(); "
        "    return C.Roll(rr); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "`TotallyBogusMember` " + kMsg));
}

// ── (d) the property-getter face ────────────────────────────────────────────

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           bogus_member_in_a_property_getter_is_rejected_spec_22_2) {
    // §22.2: a property getter always yields a value, so a getter body that
    // lowered to nothing is unambiguously broken. Before this it took the
    // silent zero/null degrade behind the fourth copy of the #2491 note.
    const std::string program =
        "module M; import System.Diagnostics; "
        "class Holder "
        "  public owned _r: Stopwatch; "
        "  public constructor() _r := new Stopwatch(); end constructor; "
        "  public property Roll: int => _r.TotallyBogusMember(1, 2); "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  return h.Roll; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "`TotallyBogusMember` " + kMsg));
}

// ── The other value-demanding positions #2935 never enumerated ──────────────

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           bogus_member_as_a_call_argument_is_rejected_stdlib_2_7) {
    // Neither #2935 nor #2966 lists this position. It falls out of gating at
    // the CALL rather than at a consumer — which is the argument for doing it
    // there: an enumeration of consumers is a positive list, and a positive
    // list missing a name fails by accepting.
    const std::string program =
        "module M; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var r := new Stopwatch(); "
        "  Console.WriteLine(r.TotallyBogusMember(1, 2)); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "`TotallyBogusMember` " + kMsg));
}

// ── ANTI-OVER-REJECT controls — the shapes a value-blind gate rejected ──────

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           unit_returning_builtin_bare_statement_still_lowers_spec_30_15) {
    // `ChannelSender<T>.Send` (stdlib §6.9 / spec §30.15) reaches the very same
    // fallthrough with no value — and emits `bz_channel_sender_send` on the way,
    // which is what excludes it. Oracle: the channel's buffered `Count`.
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

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           unit_returning_builtin_arrow_body_still_lowers_spec_30_15) {
    // The counterexample #2966 itself records as the one that killed the naive
    // `ci.returnType` version: a UNIT-returning arrow body over `Send` is legal
    // Bezel and works. Oracle: the item really is buffered.
    const std::string program =
        "module M; import System.Threading; "
        "class C "
        "  public static method Push(s: ChannelSender<int>) => s.Send(42); "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    C.Push(sender); "
        "    return ch.Count; "
        "  end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(1, r.exitCode);
}

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           domain_spawn_bare_statement_still_lowers_spec_30_14) {
    // The OTHER shape in the five lit programs the value-blind gate
    // false-rejected: `Domain.Spawn(…)` (spec §30.14), verbatim the canonical
    // multi-producer pattern from
    // tests/lit/sema/send_600_channel_rules__channel_sender_as_spawn_arg_accepted_30_15.bzl
    // — a built-in receiver whose member reaches the fallthrough. Oracle: the
    // spawned work really ran and its item is in the channel.
    const std::string program =
        "module M; import System.Threading; "
        "class Worker "
        "  public static method ProcessAll(owned tx: ChannelSender<int>) "
        "    tx.Send(7); tx.Dispose(); "
        "  end ProcessAll; "
        "end Worker; "
        "class C "
        "  public static async method Go(ch: Channel<int>): Task "
        "    using var t := Domain.Spawn(Worker.ProcessAll, ch.NewSender()); "
        "    await t; "
        "  end Go; "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var g := C.Go(ch); "
        "    g.Wait(); "
        "    return ch.Count; "
        "  end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(1, r.exitCode);
}

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           real_random_api_in_every_position_still_works_stdlib_2_7) {
    // The positive control for all four positions at once: the SAME shapes with
    // stdlib §2.7's real surface must compile and produce real values.
    const std::string program =
        "module M; "
        "class Holder "
        "  public owned _r: Random; "
        "  public constructor() _r := new Random(); end constructor; "
        "  public property Roll: int => _r.Next(10, 20); "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Draw(r: Random): int => r.Next(10, 20); "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    using var r := new Random(); "
        "    var bad := 0; "
        "    r.Next(0, 5); "
        "    for var i in 0..<20 do "
        "      if C.Draw(r) < 10 or C.Draw(r) >= 20 then bad := bad + 1; end; "
        "      if h.Roll < 10 or h.Roll >= 20 then bad := bad + 1; end; "
        "    end; "
        "    return bad; "
        "  end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(builtin_unlowered_member_all_positions_2966,
           object_typed_receiver_stays_permissive_spec_3_605) {
    // §3/#605's permissive acceptance is untouched: the receiver is a Primitive
    // TypeRef, not a registry built-in, so the gate cannot see it. This is the
    // shape #2434's own corpus check found six live programs relying on.
    const std::string program =
        "module M; "
        "class C "
        "  public method M(o: Object): int var s := o.Anything(); return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}
