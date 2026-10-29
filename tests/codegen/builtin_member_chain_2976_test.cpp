// Regression tests for issue #2976 (stdlib closed surfaces, stdlib §6.5
// `EventSource<T>`; language spec §3/#605 permissive typing, v0.240): a member
// that DOES NOT EXIST, reached as an intermediate HOP of a member chain on a
// built-in receiver, compiled clean and SILENTLY OMITTED THE WORK.
//
//   using var src := new EventSource<int>();
//   using var sub := src.Event.Subscribe(Handle);   // `Event` names nothing
//   src.Raise(7);
//
// printed the trailing line and exited 0 with the handler NEVER FIRED, behind
// #2434's `internal:` warning. stdlib §6.5 declares `EventSource<T>`'s whole
// surface — `Subscribe`/`Raise`/`Add`/`Remove` and a destructor — so `src.Event`
// names nothing; the chain then applied a REAL member (`Subscribe`) to that
// missing value and the entire initializer degraded to a zero-initialized slot.
//
// WHY #2935 DID NOT ALREADY COVER IT, which is the whole content of this fix.
// #2935's gate is a four-fact conjunction: (1) the receiver is a name the
// `builtinTypeArity` registry resolves without a user `TypeDecl`, (2) every
// lowering arm declined, (3) SEMA did not type the call either, and (4) the
// position DEMANDS A VALUE. (2), (3) and (4) all hold here. (1) did not — not
// because the receiver is not a built-in, but because the predicate asked about
// `call->callee`'s IMMEDIATE target, and that target is `src.Event`: a member
// ACCESS sema never typed, not a built-in NAME. The built-in (`EventSource`) is
// one hop further down.
//
// The fix walks DOWN the member-access spine and stops at the first hop sema
// typed, which keeps conjunct (3) at EVERY hop rather than only the last. A
// chain whose inner hop sema DID type is therefore left exactly as before — the
// failure, if any, is above that hop and this predicate must not blame the
// inner name for it. `Guid.Empty.ToString()` (below) is that control.
//
// ⚠ THE DIAGNOSTIC MUST NAME THE RIGHT HOP. `Subscribe` is documented AND
// implemented (the control below proves it fires); a message blaming it would
// be a correct rejection carrying a false claim about the program. The message
// names `Event`, and says in so many words that naming `Subscribe` would be
// wrong. Both halves are pinned.
//
// FALSE-REJECT COST, measured rather than reviewed: the full lit corpus
// (2096 tests) is green with this change, and the two-hop static chains that
// DEGRADE today (`DateTime.Now.ToString()`) keep their #2434 warning rather
// than becoming errors — the descent stops at a root that is not a Named
// built-in TYPE, exactly as before.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by exit code alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

// #2976's filed repro, verbatim in shape: the non-existent `.Event` hop between
// the source and a real `Subscribe`.
const char* kChainRepro =
    "module M; import System.Threading; "
    "class C "
    "  public static method Main(): int "
    "    using var src := new EventSource<int>(); "
    "    using var sub := src.Event.Subscribe(Handle); "
    "    src.Raise(7); "
    "    Console.WriteLine(\"reached end\"); "
    "    return 0; "
    "  end Main; "
    "  public static method Handle(e: int) "
    "    Console.WriteLine(\"handler fired\"); "
    "  end Handle; "
    "end C;";

}  // namespace

// ── The filed repro ─────────────────────────────────────────────────────────

BEZEL_TEST(builtin_member_chain_2976,
           nonexistent_hop_before_a_real_member_is_rejected_stdlib_6_5) {
    // #2966 (v0.240) moved WHERE this fires without changing WHETHER it does:
    // the general call-site gate now reports it before the local-declaration
    // consumer gets to, so the clause after the family's shared opening names
    // the CAUSE ("no dispatch arm emitted any code") instead of the consumer.
    // The opening, the hop attribution and the rejection are unchanged.
    BEZEL_EXPECT_TRUE(hasError(
        kChainRepro,
        "`Event` on built-in type `EventSource` produced no value, and no "
        "dispatch arm emitted any code for this call"));
}

BEZEL_TEST(builtin_member_chain_2976,
           the_handler_no_longer_silently_fails_to_subscribe_stdlib_6_5) {
    // The ORACLE for the defect is not exit 0 — the pre-fix program exited 0.
    // It is that the program no longer SHIPS: before this fix `bezelc` emitted
    // a warning and a working binary that printed "reached end" without ever
    // printing "handler fired". Assert the compile is now rejected AND that no
    // such binary ran.
    auto r = runAot(kChainRepro);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.stdoutText.find("handler fired") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("reached end") == std::string::npos);
}

// ── The diagnostic must blame the RIGHT hop, and say so ─────────────────────

BEZEL_TEST(builtin_member_chain_2976,
           diagnostic_names_the_failing_hop_not_the_real_member_applied_to_it) {
    // `Subscribe` IS on stdlib §6.5's surface and IS implemented — the control
    // below runs it. A message that blamed `Subscribe` would be a correct
    // rejection asserting something false about the program.
    BEZEL_EXPECT_TRUE(hasError(kChainRepro, "the failing hop is `Event`"));
    BEZEL_EXPECT_TRUE(hasError(kChainRepro, "was then applied to its missing "
                                            "value, so naming `Subscribe` "
                                            "here would be wrong"));
    auto r = runAot(kChainRepro);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(
            d.message.find("`Subscribe` on built-in type") == std::string::npos);
}

// The diagnostic's LOCATION (it must not be 0:0) is pinned in the lit track —
// tests/lit/builtin_member_chain_nonexistent_hop_2976.bzl — because the AOT
// gtest harness deliberately does not preserve `.location`: on a compile reject
// it packs the whole driver stderr into ONE diagnostic with an empty location
// (tests/support/aot_support.hpp, "S1: no test inspects a JitResult
// diagnostic's .location"). FileCheck sees the real `file:line:col:` prefix.

BEZEL_TEST(builtin_member_chain_2976,
           unchained_shape_keeps_2935_wording_with_no_chain_clause) {
    // #2935's own repro is the SAME predicate with nothing applied to the
    // failing hop. Its message must not grow a clause about a member that does
    // not exist in the program — pinned so the parenthetical stays conditional.
    // #7661 (stdlib §2.7, v0.350): sema now refuses an undeclared member on
    // `Random` before this codegen site, so the row uses `Stopwatch` (§8.1).
    const std::string program =
        "module M; import System.Diagnostics; "
        "class C public static method Main(): int "
        "  using var r := new Stopwatch(); "
        "  var v := r.NextInt(1, 3); "
        "  return v; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program,
                               "`NextInt` on built-in type `Stopwatch` produced "
                               "no value, and no dispatch arm emitted any code "
                               "for this call"));  // #2966's site — see above
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find("the failing hop is") ==
                          std::string::npos);
}

// ── ANTI-OVER-REJECT controls ───────────────────────────────────────────────

BEZEL_TEST(builtin_member_chain_2976,
           the_correct_spelling_still_subscribes_and_fires_stdlib_6_5) {
    // The control from #2976's own report. The ORACLE is that the handler
    // FIRES — `Subscribe` is real, and this fix must not touch it.
    const std::string program =
        "module M; import System.Threading; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new EventSource<int>(); "
        "    using var sub := src.Subscribe(Handle); "
        "    src.Raise(7); "
        "    Console.WriteLine(\"reached end\"); "
        "    return 0; "
        "  end Main; "
        "  public static method Handle(e: int) "
        "    Console.WriteLine(\"handler fired\"); "
        "  end Handle; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("handler fired") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("reached end") != std::string::npos);
}

BEZEL_TEST(builtin_member_chain_2976,
           a_typed_inner_hop_on_a_builtin_still_lowers_stdlib_2_6) {
    // The descent's stopping rule, exercised on a real two-hop spine rooted at
    // a built-in: `Guid.Empty` (stdlib §2.6) is typed by sema, so the walk
    // never descends past it and the chain lowers exactly as before. Oracle is
    // the VALUE — the canonical all-zero Guid is 36 characters.
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var g := Guid.Empty.ToString(); "
        "  Console.WriteLine(g); "
        "  return g.Length; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(36, r.exitCode);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("00000000-0000-0000-0000-000000000000") !=
        std::string::npos);
}

BEZEL_TEST(builtin_member_chain_2976,
           object_typed_root_stays_permissive_spec_3_605) {
    // §3/#605's permissive surface, in the CHAINED shape this fix newly walks:
    // the spine bottoms out at an `Object`-typed receiver, which is a Primitive
    // TypeRef and not a registry built-in, so the descent must find nothing and
    // leave the program compiling exactly as before.
    const std::string program =
        "module M; "
        "class C "
        "  public method M(o: Object): int var s := o.Anything.More(); "
        "  return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(builtin_member_chain_2976,
           user_type_sharing_a_builtin_name_is_rejected_as_a_user_type_spec_11_3) {
    // A user class may legitimately be named `EventSource`. It carries a real
    // `decl`, so the descent's built-in test must not claim it — pinned because
    // the walk makes the predicate see MORE receivers than before, so the
    // `decl`-carrying exclusion now has to hold at the hop the walk STOPS at
    // too, not only at the callee's immediate target.
    //
    // The oracle is WHICH diagnostic fires: sema's known-complete-user-type
    // "no member" (§11.3/§12.1), never this fix's built-in wording.
    //
    // ⚠ Deliberately a REJECT program, not a running one. A user class named
    // `EventSource` bound to a `using var` SIGSEGVs on this base for an
    // unrelated reason — `registerUsingDisposal`'s §6.5 arm has three detection
    // branches and the NewExpr-name one omits the `!rt->declaration` guard the
    // other two carry, so the user object is freed with
    // `bz_event_source_free`. Measured on this fix's base and filed separately;
    // this test must not depend on it either way.
    const std::string program =
        "module M; "
        "class EventSource "
        "  public readonly property Event: string := \"abcdefg\"; "
        "end EventSource; "
        "class C public static method Main(): int "
        "  using var src := new EventSource(); "
        "  return src.Bogus.Anything(); "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `Bogus` on type `EventSource`"));
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find("on built-in type `EventSource`") ==
                          std::string::npos);
}
