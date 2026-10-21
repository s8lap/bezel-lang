// Regression tests for #5272-ff — the fix-forward on #5272, covering the SECOND
// text-keyed chokepoint.
//
// #5272 fixed the static-CALL ladder (`LlvmBackend_Calls.cpp`'s
// `tryLowerBuiltinClassCall`, whose ~24 `cls == "<Builtin>"` arms derive `cls`
// from the receiver identifier's SPELLING). It did NOT reach the
// static-PROPERTY ladder: `LlvmBackend_Exprs.cpp`'s `lowerExpr` matches the
// same way, and SEMA's `resolveMemberAccess` TYPES those reads as the builtin
// and returns early. So after #5272 a user class named after a builtin still
// lost its own static PROPERTY — silently, with no diagnostic.
//
// WHAT THE DOCUMENTS REQUIRE. Spec §2.3 (v0.313, *Always-available names*): of
// the always-available names, only §3.4's reserved row — the PascalCase aliases
// of the primitive value types, plus `String` — cannot be shadowed; "every
// other always-available name may be shadowed by local declarations under the
// normal name-resolution rules". Spec §2.7 (v0.313, *Name resolution for dotted
// names*, rule 1) supplies those rules: resolve the leftmost segment `A` as a
// bare name — local scope, then the enclosing type's accessible members, then
// "the current module's declarations", then unambiguous imported bare names,
// then import aliases — and "if `A` resolves to a type, the rest of the chain
// resolves against that type". The current-module declaration therefore wins,
// and the always-available name stays reachable by its qualified spelling
// (`System.Math.Sqrt`). `Math`, `Environment`, `DateTime`, `TimeSpan`,
// `IntPtr`, `UIntPtr`, `Guid` and `CancellationToken` are in neither §3.4
// reserved set, so a user class may take those spellings and MUST win.
//
// `int`/`long`/`double` are the counter-example and are deliberately NOT gated:
// §3.4 reserves them, so no user class can carry those spellings.
//
// MEASURED PRE-FIX (staging @ 23d1c7ad, `agent_check --probe --asan`; one
// user class per probe, the USER's declared value in the right column):
//
//     user class            member        user's value   printed (the builtin)
//     Math                  Pi            3.25            3.141592653589793
//     Math                  Tau           6.5             6.283185307179586
//     Environment           ProcessorCount 555             20
//     DateTime              Now           111             639265989418842169
//     DateTime              MaxValue      222             3155378975999999999
//     TimeSpan              Zero          333             00:00:00
//     IntPtr                Zero          444             0
//     Guid                  Empty         777             00000000-0000-...
//     CancellationToken     None          888             System.Threading...
//
// Not one crashed and not one warned: every row is a SILENT wrong answer. That
// is why each row below asserts the USER's exact stdout bytes rather than
// "did not crash" — a `didNotCrash` assertion would pass against the defect.
//
// The fix is one shared predicate, `sema::isUserDeclaredTypeSym`
// (src/sema/Symbol.hpp), read through `Emitter::receiverIsUserType`. It is the
// EXISTING #2997 discriminator codegen's `builtinTypeName` documents — a
// `builtinClassSym` is shaped `decl == nullptr && moduleName.empty()` — and it
// is consulted by SEMA's static-member typing and by CODEGEN's static-property
// and static-call lowering, so the two layers cannot disagree about whether a
// receiver names a built-in.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// One user class declared OUTSIDE Main (so its name is in scope for a static
// read) plus the usual Main. `decl` is the user's class.
std::string prog(const std::string& decl, const std::string& body) {
    return std::string("module M; ") + decl +
           " public class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

void expectOut(const std::string& decl, const std::string& body,
               const std::string& want) {
    auto r = runAot(prog(decl, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// ---- #5272-ff: the user class shadows the builtin's static PROPERTY -------
//
// Each row uses a `property` (an expression-bodied static auto-property),
// NOT a `const`. That distinction is load-bearing and was measured: a user
// `Math` with `static const PI: double := 3.14` was ALREADY correct pre-fix —
// the `const` read path is not text-keyed — so a `const`-based row would be
// VACUOUS against this defect. The `property` spelling is the one that fails.

BEZEL_TEST(user_class_shadows_builtin_property_5272, math_pi_returns_user_value) {
    // Pre-fix: 3.141592653589793 (the builtin's π). The user's property never
    // ran and nothing said so.
    expectOut("public class Math public static property Pi: double => 3.25; "
              "end Math; ",
              "Console.WriteLine(Math.Pi);",
              "3.25\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272, math_tau_returns_user_value) {
    // The second `Math` constant, so a fix that gated only `Pi` is caught.
    expectOut("public class Math public static property Tau: double => 6.5; "
              "end Math; ",
              "Console.WriteLine(Math.Tau);",
              "6.5\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           environment_processor_count_returns_user_value) {
    // Pre-fix: 20 (the host's real nproc).
    expectOut("public class Environment public static property "
              "ProcessorCount: int => 555; end Environment; ",
              "Console.WriteLine(Environment.ProcessorCount);",
              "555\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272, datetime_now_returns_user_value) {
    // Pre-fix: 639265989418842169 — a TICK COUNT, the builtin's clock. This is
    // the row that most plainly shows the substitution is a real wrong answer
    // and not a coincidence of a similarly-shaped constant.
    expectOut("public class DateTime public static property Now: int => 111; "
              "end DateTime; ",
              "Console.WriteLine(DateTime.Now);",
              "111\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           datetime_max_value_returns_user_value) {
    // Pre-fix: 3155378975999999999 (the builtin's MaxValue).
    expectOut("public class DateTime public static property MaxValue: int => 222; "
              "end DateTime; ",
              "Console.WriteLine(DateTime.MaxValue);",
              "222\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272, timespan_zero_returns_user_value) {
    // Pre-fix: 00:00:00 — the builtin's zero TimeSpan, rendered through the
    // builtin's own ToString, so the type of the wrong answer differs too.
    expectOut("public class TimeSpan public static property Zero: int => 333; "
              "end TimeSpan; ",
              "Console.WriteLine(TimeSpan.Zero);",
              "333\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272, intptr_zero_returns_user_value) {
    // Pre-fix: 0 — the null pointer. §32.7's builtin `IntPtr.Zero`.
    expectOut("public class IntPtr public static property Zero: int => 444; "
              "end IntPtr; ",
              "Console.WriteLine(IntPtr.Zero);",
              "444\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272, uintptr_zero_returns_user_value) {
    // The `IntPtr` arm's twin; both spellings must be gated or a fix that
    // gated only `IntPtr` is caught.
    expectOut("public class UIntPtr public static property Zero: int => 444; "
              "end UIntPtr; ",
              "Console.WriteLine(UIntPtr.Zero);",
              "444\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272, guid_empty_returns_user_value) {
    // Pre-fix: 00000000-0000-0000-0000-000000000000 — stdlib §2.6's all-zero
    // Guid, rendered as a Guid although the user's property is an `int`.
    expectOut("public class Guid public static property Empty: int => 777; "
              "end Guid; ",
              "Console.WriteLine(Guid.Empty);",
              "777\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           cancellation_token_none_returns_user_value) {
    // Pre-fix: System.Threading.CancellationToken — §30.8's token singleton.
    expectOut("public class CancellationToken public static property None: int => 888; "
              "end CancellationToken; ",
              "Console.WriteLine(CancellationToken.None);",
              "888\n");
}

// The CALL form of the same names. §2.7 resolves the head and then looks the
// member up ON THE USER'S TYPE, so `DateTime.Now()` is the user's method and
// must never reach a builtin arm. Pre-fix this emitted warning #3079 ("the
// argument of this `Console.WriteLine` failed to lower to a value") and printed
// nothing: the ladder's silent substitution had been turned into a loud
// lower-fail by #5272, which is an improvement but still not the user's value.
BEZEL_TEST(user_class_shadows_builtin_property_5272,
           datetime_now_call_resolves_to_user_method) {
    expectOut("public class DateTime public static method Now(): long "
              "return 4242; end Now; end DateTime; ",
              "Console.WriteLine(DateTime.Now());",
              "4242\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           datetime_utcnow_call_resolves_to_user_method) {
    expectOut("public class DateTime public static method UtcNow(): long "
              "return 4343; end UtcNow; end DateTime; ",
              "Console.WriteLine(DateTime.UtcNow());",
              "4343\n");
}

// A user class whose name is NOT a builtin, declaring a member whose NAME
// collides with a builtin static property. The receiver-name guard must key on
// the resolved SYMBOL, not on the member spelling, or this row would be the
// false positive the guard is most likely to introduce.
BEZEL_TEST(user_class_shadows_builtin_property_5272,
           non_builtin_class_with_colliding_member_unaffected) {
    expectOut("public class Widget public static property Now: int => 111; "
              "public static property Zero: int => 222; end Widget; ",
              "Console.WriteLine(Widget.Now); Console.WriteLine(Widget.Zero);",
              "111\n222\n");
}

// A user class named after a builtin, declaring a member that does NOT collide
// with any builtin static property. Pre-fix and post-fix alike this returned the
// user's value, so it is a control: it proves the fix does not break an
// already-correct member read on the same class.
BEZEL_TEST(user_class_shadows_builtin_property_5272,
           builtin_named_class_non_colliding_member_unaffected) {
    expectOut("public class DateTime public static property Widget: int => 111; "
              "end DateTime; ",
              "Console.WriteLine(DateTime.Widget);",
              "111\n");
}

// ---- controls: the builtins still lower when NOTHING shadows them ---------
//
// The fix must not disarm the text-keyed arms. Without these it could pass by
// simply never reaching a builtin arm.

BEZEL_TEST(user_class_shadows_builtin_property_5272, builtin_math_pi_still_lowers) {
    // Not vacuous: π to 16 digits, so a wrong constant cannot pass.
    expectOut("", "Console.WriteLine(Math.Pi);", "3.141592653589793\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272, builtin_math_tau_still_lowers) {
    expectOut("", "Console.WriteLine(Math.Tau);", "6.283185307179586\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           builtin_datetime_max_value_still_lowers) {
    expectOut("", "Console.WriteLine(DateTime.MaxValue);", "9999-12-31T23:59:59\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           builtin_timespan_zero_still_lowers) {
    expectOut("", "Console.WriteLine(TimeSpan.Zero);", "00:00:00\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           builtin_intptr_zero_still_lowers) {
    expectOut("", "Console.WriteLine(IntPtr.Zero);", "0\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           builtin_guid_empty_still_lowers) {
    expectOut("", "Console.WriteLine(Guid.Empty);",
              "00000000-0000-0000-0000-000000000000\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           builtin_datetime_now_still_lowers) {
    // The builtin `DateTime.Now` is a tick count, so it is machine- and
    // run-dependent. Asserting a literal would be flaky; what is asserted is
    // that it is POSITIVE and enormous — i.e. the builtin's clock, which is the
    // one thing the user class rows above must NOT produce. A user `Now`
    // returns a small value (111/4242), so this control separates the two
    // outcomes without pinning a value that changes every run.
    expectOut("", "var n: long := DateTime.Now.Ticks; "
                  "Console.WriteLine(n > 1000000000000L);",
              "True\n");
}

BEZEL_TEST(user_class_shadows_builtin_property_5272,
           builtin_environment_processor_count_still_lowers) {
    // The builtin is the host's real nproc, so assert only that it is a
    // positive count that is NOT the user's 555 — the shape of a control that
    // cannot go stale and still distinguishes the two lowerings.
    expectOut("", "var n: int := Environment.ProcessorCount; "
                  "Console.WriteLine(n > 0); Console.WriteLine(n = 555);",
              "True\nFalse\n");
}
