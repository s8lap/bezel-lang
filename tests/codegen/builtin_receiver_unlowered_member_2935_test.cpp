// Regression tests for issue #2935 (stdlib closed surfaces; language spec
// §3/#605 permissive typing, §21.7, v0.240): a member that DOES NOT EXIST, on
// a built-in receiver, compiled clean and evaluated to 0.
//
//   using var r := new Random();
//   var v := r.TotallyBogusMember(1, 2);   -> internal: … #2434, exit 0
//
// #2935 was found by accident, by writing `Random.NextInt(1, 3)` — a name the
// stdlib does not have (§2.7 declares `Next(minValue, maxValue)`) — and having
// the compiler accept it. For a numeric result the degraded 0 is a plausible
// value, so nothing downstream necessarily looks wrong; and it composes with
// #2922 (the same zero-degrade producing a null `string`) and #2921 into
// memory-unsafety rather than mere wrongness.
//
// MEASURED POPULATION, on `f069645`, over all 18 built-in receivers × the same
// bogus member: FIVE accepted it — `Random`, `StringBuilder` and `Stopwatch`
// (the three #2935 lists) plus `OwningBuffer<T>` and `CancellationTokenSource`.
// The receivers that already rejected (`Mutex`, `Atomic<T>`, `Channel<T>`,
// `Dictionary<K,V>`, …) do so through hand-written closed-surface arms in sema,
// one per type. That the issue's own enumeration was already short by two is
// the direct evidence against fixing this by adding three more names, so the
// gate here is a PROPERTY instead:
//
//   the receiver is a name the compiler's `builtinTypeArity` registry resolves
//   without any user `TypeDecl` (via `sema::isBuiltinTypeName`, the single
//   source of truth for that), AND every lowering arm declined, AND SEMA never
//   typed the call either, AND the position DEMANDS A VALUE.
//
// A built-in's member surface is closed by construction — there is no
// declaration a user could extend — so the first two conjuncts mean "this
// member is not on that surface". Reading the defining registry rather than
// restating part of it is what covers a built-in added later for free.
//
// ⚠ THE LAST TWO CONJUNCTS ARE NOT DECORATION — each was forced by a WORKING
// program a simpler gate rejected, found by measurement rather than review.
// `lowerCallExpr` returns nullptr BOTH
// for "no arm lowered this" and for "an arm lowered it and the result is unit"
// — `tryLowerChannelAndDomainCall`'s `Send` arm emits a real
// `bz_channel_sender_send` and then returns nullptr, and execution continues to
// the same fallthrough (filed as #2963). Gating on the first two conjuncts
// alone was the first version of this fix and false-rejected five green lit
// programs. The controls at the bottom pin both unit-returning shapes that
// proved it. A THIRD counterexample forced the sema conjunct: #2167's bound
// drain view (`var d := arr.Drain();`) binds a deliberately DEAD slot, so "the
// initializer produced no value" is not by itself "a value was demanded". The
// positions still uncovered — a bare expression statement, and the three
// sibling #2491 return-degrade sites — are filed as #2966.
//
// SEMA CANNOT MAKE THIS CALL, and that is measured too, not preferred: #2465's
// comment records the bug it paid for — "`!ctx_.exprType(ma)` is NOT a reliable
// signal for 'nothing implements this' — it is also the signature of 'codegen
// implements this by receiver-name matching and never needed sema to type it'"
// — and `Array<T>.GetHashCode()` is exactly that shape, and works. Only past
// every dispatch arm is "no lowering exists" a fact.

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

// The stable part of the diagnostic. Deliberately NOT "no member `X`": that
// wording asserted the member does not exist, which is false for a documented
// member that merely yields nothing — see
// `unit_returning_builtin_bound_to_local_rejected_without_claiming_the_member_is_absent`.
const char* kNoMember = "on built-in type `";

// Position A — the filed repro: a LOCAL DECLARATION demands a value.
std::string bogusInLocalDecl(const char* ctor) {
    return std::string(
        "module M; import System.Threading; "
        "class C public static method Main(): int "
        "  using var x := ") + ctor + "; "
        "  var v := x.TotallyBogusMember(1, 2); "
        "  Console.WriteLine(\"compiled\"); "
        "  return 0; "
        "end Main; end C;";
}

}  // namespace

// ── The filed repro, and its origin ─────────────────────────────────────────

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           bogus_member_on_random_rejected_in_local_decl_stdlib_2_7) {
    BEZEL_EXPECT_TRUE(hasError(bogusInLocalDecl("new Random()"), kNoMember));
}

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           the_typo_that_found_this_is_rejected_stdlib_2_7) {
    // `Random.NextInt(1, 3)` — the exact spelling that surfaced #2935. stdlib
    // §2.7's real API is `Next(minValue, maxValue)`; `NextInt` does not exist.
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var r := new Random(); "
        "  var v := r.NextInt(1, 3); "
        "  return v; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program,
                               "`NextInt` on built-in type `Random` produced "
                               "no value, so it cannot initialize this local"));
}

// ── The rest of the MEASURED population, including the two the issue missed ──

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           bogus_member_on_stringbuilder_rejected_stdlib_3_1) {
    BEZEL_EXPECT_TRUE(
        hasError(bogusInLocalDecl("new StringBuilder()"), kNoMember));
}

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           bogus_member_on_stopwatch_rejected_stdlib_8_1) {
    BEZEL_EXPECT_TRUE(
        hasError(bogusInLocalDecl("new Stopwatch()"), kNoMember));
}

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           bogus_member_on_owningbuffer_rejected_stdlib_4_14) {
    // NOT in #2935's list — found by probing the whole built-in population for
    // this fix. A receiver-name enumeration would have left it accepting.
    BEZEL_EXPECT_TRUE(
        hasError(bogusInLocalDecl("new OwningBuffer<int>(4)"), kNoMember));
}

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           bogus_member_on_cancellationtokensource_rejected_stdlib_6_5) {
    // Likewise not in #2935's list.
    BEZEL_EXPECT_TRUE(
        hasError(bogusInLocalDecl("new CancellationTokenSource()"), kNoMember));
}

// ── The second value-demanding position ─────────────────────────────────────

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           bogus_member_rejected_in_return_position_spec_21_7) {
    // Measured as a residual of the first draft (which covered only the local
    // declaration): in return position the same typo still degraded to a silent
    // 0 behind an `internal: … #2491` warning, and the program ran to exit 0.
    // §21.7 supplies the "a value is demanded" half as a language rule — "this
    // callable has no return type — `return` may not carry a value" — so a
    // `return expr;` reaching codegen is in a callable that has a return type.
    const std::string program =
        "module M; "
        "class C "
        "  public static method Get(r: Random): int "
        "    return r.TotallyBogusMember(1, 2); "
        "  end Get; "
        "  public static method Main(): int "
        "    using var rr := new Random(); "
        "    return C.Get(rr); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "cannot supply this return value"));
}

// ── The diagnostic must not assert something FALSE ──────────────────────────

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           unit_returning_builtin_bound_to_local_rejected_without_claiming_the_member_is_absent) {
    // Measured while re-landing this fix on a base that had gained #2915's
    // unit-typed exemption. It would be natural to assume that a UNIT-returning
    // built-in member call is #2915's business and cannot reach this gate.
    // It is not: sema does not type `ChannelSender<T>.Send` unit, it does not
    // type it AT ALL, so this lands here.
    //
    // REJECTING IT IS CORRECT — `Send` (stdlib §6.9) yields nothing, so it
    // cannot initialize a local, and before this fix `x` was silently
    // zero-initialized. But an earlier wording said "no member `Send` on
    // built-in type `ChannelSender`", which is FALSE: `Send` is documented AND
    // implemented (it emits `bz_channel_sender_send`). This pins BOTH halves —
    // the rejection happens, and the message does not claim the member is
    // absent — because the second half is the kind of thing that silently
    // decays back.
    const std::string program =
        "module M; import System.Threading; "
        "class C public static method Main(): int "
        "  using var ch := new Channel<int>(); "
        "  using var sender := ch.NewSender(); "
        "  var x := sender.Send(42); "
        "  return ch.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "produced no value, so it cannot "
                                        "initialize this local"));
    // The message must offer BOTH readings, not assert absence.
    BEZEL_EXPECT_TRUE(hasError(program, "or it is documented but yields "
                                        "nothing here"));
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find("no member `Send`") ==
                          std::string::npos);
}

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           unit_typed_initializer_stays_exempt_spec_4_7_2915) {
    // #2915's neighbour in the merged guard: `()` lowers to LLVM `void`, so a
    // unit-typed initializer legitimately has no lowered value — that is the
    // type's representation, not a lowering failure. It must reach neither this
    // fix's Error nor #2434's Warning. Disjoint from the gate above by
    // construction (this HAS a sema type; that has none), pinned so the
    // merge order can never quietly start mattering.
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var u: () := (); "
        "  var o: Object := (); "
        "  Console.WriteLine(o.ToString()); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("()") != std::string::npos);
}

// ── ANTI-OVER-REJECT controls ───────────────────────────────────────────────

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           real_random_api_still_works_stdlib_2_7) {
    // stdlib §2.7's actual surface. The oracle is the VALUE, not exit 0: the
    // result must land inside the requested range on every draw.
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var r := new Random(); "
        "  var bad := 0; "
        "  for var i in 0..<50 do "
        "    var v := r.Next(10, 20); "
        "    if v < 10 or v >= 20 then bad := bad + 1; end; "
        "  end; "
        "  var d := r.NextDouble(); "
        "  if d < 0.0 or d >= 1.0 then bad := bad + 1; end; "
        "  return bad; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           real_stringbuilder_api_still_works_stdlib_3_1) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"ab\"); "
        "  sb.Append(\"cde\"); "
        "  var s := sb.ToString(); "
        "  Console.WriteLine(s); "
        "  return s.Length; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(5, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("abcde") != std::string::npos);
}

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           unit_returning_builtin_statement_still_lowers_spec_30_15) {
    // The false-reject the FIRST draft of this fix caused, kept as a permanent
    // control: `ChannelSender<T>.Send` (stdlib §6.9 / spec §30.15) is fully
    // implemented, emits `bz_channel_sender_send`, and then returns nullptr
    // because the result is unit — reaching the very fallthrough a
    // value-blind gate would report from. The oracle is the channel's buffered
    // `Count`, not exit 0.
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

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           unit_returning_builtin_arrow_body_still_lowers_spec_30_15) {
    // The OTHER measured counterexample, and the reason the three sibling
    // #2491 return-degrade sites were deliberately left alone: a UNIT-returning
    // ARROW BODY is legal Bezel that §21.7 does not cover, and it reaches the
    // arrow-body #2491 site with no value because `Send` lowered as unit. The
    // same gate applied there would reject this working program (#2966 tracks
    // the `ci.returnType` test those sites need). Oracle: the item really is
    // buffered.
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

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           bound_drain_view_still_lowers_stdlib_4_5_2167) {
    // The THIRD measured counterexample, and the one that forced the "sema is
    // as ignorant as codegen" conjunct into the predicate: #2167's BOUND drain
    // view binds a local whose slot is DELIBERATELY DEAD. `Drain()` is real
    // (stdlib §4.5), documented, and implemented — but implemented in the
    // for-in, not as a value: `lowerOwnedSeqDrainFor` recognises the bound form
    // by walking back to `d`'s LocalDecl initializer and lowers the loop
    // through the receiver, so no value is ever needed in the slot.
    //
    // "The initializer produced no value" is therefore NOT on its own the
    // property "a value was demanded" — this program is correct and reaches
    // that degrade. Requiring `ctx_.exprType(call)` to ALSO be absent excludes
    // it exactly, because sema types `Drain()` `IOwningEnumerable<T>` while a
    // member that does not exist is typed by nobody.
    //
    // Oracle: the disposal count (§4.5 — the view disposes every element
    // exactly once), not exit 0.
    const std::string program =
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  var d := arr.Drain(); "
        "  for var item in d do "
        "    using var u := item; "
        "  end; "
        "  return R._n; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           object_typed_receiver_stays_permissive_spec_3_605) {
    // §3/#605's permissive acceptance, byte-for-byte the shape of
    // tests/lit/sema/sema_unknown_member__object_typed_receiver_permissive.bzl
    // and of #2434's own control: an unknown member on an `Object`-typed
    // receiver is accepted BY DESIGN and reaches the same zero-degrade. It must
    // keep compiling — the receiver is a Primitive TypeRef, not a registry
    // built-in, so the gate cannot see it.
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

BEZEL_TEST(builtin_receiver_unlowered_member_2935,
           user_type_sharing_a_builtin_name_is_not_caught_spec_11_3) {
    // A user class may legitimately be named `Random`. It carries a real
    // `decl`, so it is not the synthetic registry built-in and its members
    // resolve normally — this must compile and return the real value, not be
    // swept up by a name test.
    const std::string program =
        "module M; "
        "class Random public method Roll(): int return 4; end Roll; end Random; "
        "class C public static method Main(): int "
        "  using var r := new Random(); "
        "  return r.Roll(); "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(4, r.exitCode);
}
