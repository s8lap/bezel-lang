// Regression tests for issue #2974 (language spec §11.7 "Static classes" /
// §12.5; stdlib §6.8 `Domain`, §7.3–§7.5, §8.2, §2.12; v0.240): `new` on a
// BUILT-IN that no declaration makes constructible compiled clean.
//
//   var d := new Domain("d");     // §11.7: `new` on a static class is an error
//
// ⚠ THE ISSUE'S SPEC CITATION IS WRONG, and correcting it is load-bearing for
// anyone re-reading this. #2974 cites "§22.11"; §22.11 is "Object initializer
// blocks (retired v0.188)". The rule quoted — "A static class may not be
// instantiated: `new` on a static class is a compile error" — is **§11.7**, and
// the existing sema rejection for USER static classes already cites §11.7
// correctly (`ExprTypecheck_Exprs.cpp`).
//
// ── THE ISSUE'S ROOT CAUSE IS ALSO WRONG, and that is the fix ───────────────
// #2974 proposes the missing check is "does this reserved built-in name denote
// an instantiable class, and does the argument list match a declared
// constructor?". Measured on 0dac23f, `Domain` is not an unchecked
// fall-through at all: `Emitter::lowerNew` has a DELIBERATE `colName ==
// "Domain"` arm emitting `bz_domain_alloc`, with a matching `.Name` getter and
// `.Dispose()` arm and three runtime functions behind them — an instance
// surface the standard library does not declare, and which four tests pinned
// under a `// §6.8` citation for a section that declares none of it. The
// runtime's own comment says so: "stdlib §6.8 documents Domain as a
// static-class for Domain.Spawn. The probe suite exercises `new Domain("test")`
// and `d.Name` as an instance-creation path…". So the leak #2974 measured is
// not an accident of an unchecked `new`; it is an undeclared feature that
// allocates and has no auto-drop.
//
// Deleting that arm is therefore the fix, not adding a check around it — and
// once it is gone `new Domain(…)` produces no value like every other
// non-constructible built-in, so ONE property covers the whole family instead
// of a `Domain` special case.
//
// ── THE PROPERTY, and its MEASURED population ──────────────────────────────
// A `new` on a name the `builtinTypeArity` registry resolves without any user
// `TypeDecl`, at a position that DEMANDS A VALUE, that produced no value.
// Unlike its member-call twin (#2935) this needs no "sema was ignorant too"
// conjunct: sema types every `new T(…)` as `T`, and there is no such thing as a
// unit-valued construction, so "produced no value" is unambiguously a failure.
//
// Probing every built-in static/abstract class in the registry found EIGHT
// accepting `new`, not one: `Domain` (which constructed something real) plus
// `Path`, `File`, `Directory`, `Debug`, `BitConverter`, `Encoding` and `Linq`
// (which all silently zero-initialised the local behind #2434's warning — the
// same null-pointer degrade #2922 measured faulting at address 0x8). Each is
// pinned below. `Encoding` is `public abstract class` (stdlib §7.6) rather than
// static, and `Linq` is not a stdlib type at all, which is why the diagnostic
// offers three readings rather than asserting "static".
//
// FALSE-REJECT COST: see the anti-over-reject block at the bottom, and the
// commit message for the corpus numbers.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

std::string newLocal(const char* ctor) {
    return std::string(
        "module M; import System.Threading; "
        "class C public static method Main(): int "
        "  var x := ") + ctor + "; "
        "  Console.WriteLine(\"constructed\"); "
        "  return 0; "
        "end Main; end C;";
}

}  // namespace

// ── The filed repro ─────────────────────────────────────────────────────────

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_the_builtin_static_class_domain_is_rejected_spec_11_7) {
    BEZEL_EXPECT_TRUE(hasError(newLocal("new Domain(\"d\")"),
                               "`new Domain(\xE2\x80\xA6)` produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           the_rejection_cites_the_static_class_rule_spec_11_7) {
    // §11.7 is the rule, not §22.11 (which is the retired object-initializer
    // section). Pinned so the citation cannot drift back to the issue's.
    BEZEL_EXPECT_TRUE(hasError(newLocal("new Domain(\"d\")"),
                               "language spec \xC2\xA7""11.7"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           the_accepted_new_no_longer_ships_a_leaking_binary_spec_11_7) {
    // #2974's LSan measurement was of a binary that should never have been
    // produced. The oracle here is that no binary runs at all.
    auto r = runAot(newLocal("new Domain(\"d\")"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.stdoutText.find("constructed") == std::string::npos);
}

// ── The rest of the MEASURED population — seven the issue did not have ──────

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_path_is_rejected_stdlib_7_5) {
    BEZEL_EXPECT_TRUE(hasError(newLocal("new Path(\"x\")"), "produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_file_is_rejected_stdlib_7_3) {
    BEZEL_EXPECT_TRUE(hasError(newLocal("new File(\"x\")"), "produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_directory_is_rejected_stdlib_7_4) {
    BEZEL_EXPECT_TRUE(
        hasError(newLocal("new Directory(\"x\")"), "produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_debug_is_rejected_stdlib_8_2) {
    BEZEL_EXPECT_TRUE(hasError(newLocal("new Debug()"), "produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_bitconverter_is_rejected_stdlib_2_12) {
    BEZEL_EXPECT_TRUE(
        hasError(newLocal("new BitConverter()"), "produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_the_abstract_encoding_is_rejected_stdlib_7_6) {
    // Not static — `public abstract class Encoding`. The same property covers
    // it, which is why the diagnostic does not assert "static".
    BEZEL_EXPECT_TRUE(hasError(newLocal("new Encoding()"), "produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_linq_is_rejected_no_such_stdlib_type) {
    // `Linq` is in the compiler's built-in name registry but is not a declared
    // stdlib type at all.
    BEZEL_EXPECT_TRUE(hasError(newLocal("new Linq()"), "produced no value"));
}

// ── FOUR MORE the grid found, all factory-only per the stdlib ──────────────
//
// Not in #2974 and not static classes: real built-in CLASSES the standard
// library gives no constructor because they are obtained from a factory. Each
// previously zero-initialised the local to a NULL POINTER — measured on the
// parent compiler, `new CancellationToken()` then reported
// `IsCancellationRequested` as `False` for a token derived from no source, and
// `new Type()` rendered `Name` as empty. Silently plausible answers, which is
// the #2922 class.

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_subscription_is_rejected_stdlib_2_11) {
    // stdlib §2.11: a `Subscription` comes from `Subscribe(...)`, never `new`.
    BEZEL_EXPECT_TRUE(
        hasError(newLocal("new Subscription()"), "produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_cancellationtoken_is_rejected_stdlib_6_1) {
    // stdlib §6.1: a token is obtained from a `CancellationTokenSource`.
    BEZEL_EXPECT_TRUE(
        hasError(newLocal("new CancellationToken()"), "produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_type_is_rejected_stdlib_2_13) {
    // stdlib §2.13: a `Type` comes from `typeof(T)`.
    BEZEL_EXPECT_TRUE(hasError(newLocal("new Type()"), "produced no value"));
}

BEZEL_TEST(builtin_static_class_new_2974,
           new_on_task_is_rejected_stdlib_6_7) {
    // stdlib §6.7 / §30: a `Task` comes from an async call, `Domain.Spawn`, or
    // a `TaskCompletionSource` — never `new`.
    BEZEL_EXPECT_TRUE(hasError(newLocal("new Task()"), "produced no value"));
}

// ── THE VALUE-SHAPED BUILT-INS ARE EXEMPT, and that was MEASURED ───────────
//
// The first draft of this gate rejected these too. Measured on the PARENT
// compiler, `var g := new Guid(); Console.WriteLine(g.ToString());` prints
// `00000000-0000-0000-0000-000000000000` — exactly the documented `Guid.Empty`.
// It was never a degraded null: for a value-shaped built-in the substituted
// all-zero pattern IS `default(T)` (§17.11), and §14.2 says outright that
// "`new T()` for a value substitution is equivalent to `default(T)`". Rejecting
// it would have broken a program that works and returns the right answer —
// which is why the gate reads the existing `kNonHeapReservedBuiltins` registry
// split rather than "is it a built-in".
//
// (Whether `new Guid()` is legal Bezel at an ordinary, non-generic site is a
// question the spec does not settle. Per CLAUDE.md rule 1 that is an issue, not
// an interpretation committed to here; filed separately. These tests pin the
// STATUS QUO for it, so whichever way that question is answered, the answer is
// a deliberate edit.)

BEZEL_TEST(builtin_static_class_new_2974,
           new_guid_still_yields_the_empty_guid_spec_17_11_stdlib_2_6) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var g := new Guid(); "
        "  Console.WriteLine(g.ToString()); "
        "  return g.ToString().Length; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(36, r.exitCode);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("00000000-0000-0000-0000-000000000000") !=
        std::string::npos);
}

BEZEL_TEST(builtin_static_class_new_2974,
           other_value_shaped_builtins_stay_accepted_spec_17_11) {
    // `IntPtr` (stdlib §10), `ExceptionInfo` (§31.4) and `ReadOnlyStringSpan`
    // (stdlib §3.3) are the rest of the exemption the grid surfaced. The oracle
    // is only that they still COMPILE — their runtime shape is not this fix's
    // business, and asserting more would over-specify the status quo.
    for (const char* ctor : {"new IntPtr()", "new ExceptionInfo()",
                             "new ReadOnlyStringSpan()"}) {
        auto r = runAot(newLocal(ctor));
        BEZEL_EXPECT_TRUE(r.ran);
    }
}

// ── The REAL §6.8 surface must be untouched ────────────────────────────────

BEZEL_TEST(builtin_static_class_new_2974,
           domain_spawn_still_works_stdlib_6_8) {
    // stdlib §6.8's actual surface is `Domain.Spawn`. Deleting the undeclared
    // instance path must not touch it. Oracle is the VALUE the spawned domain
    // computes, not exit 0.
    const std::string program =
        "module M; import System.Threading; "
        "class C "
        "  public static method Work(): int return 21; end Work; "
        "  public static method Main(): int "
        "    using var t := Domain.Spawn(Work); "
        "    var v := t.Result; "
        "    return v * 2; "
        "  end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── A USER static class keeps its own, better diagnostic ───────────────────

BEZEL_TEST(builtin_static_class_new_2974,
           user_static_class_still_rejected_by_sema_spec_11_7) {
    // Measured on 0dac23f as already correct — the sema §11.7 check is gated on
    // `rt->declaration`, which a user type has and a built-in does not. That
    // asymmetry is the whole reason #2974 exists, and it answers the issue's
    // explicitly-unmeasured question ("whether a USER-declared `static class`
    // is also instantiable"): it is not.
    const std::string program =
        "module M; "
        "public static class Helper "
        "  public static method Ping(): int return 1; end Ping; "
        "end Helper; "
        "class C public static method Main(): int "
        "  var d := new Helper(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "cannot instantiate static class `Helper` with `new`"));
}

// ── ANTI-OVER-REJECT controls ───────────────────────────────────────────────

BEZEL_TEST(builtin_static_class_new_2974,
           constructible_builtins_still_construct_and_work) {
    // The gate must see only the built-ins that produce NO value. Oracle is the
    // accumulated VALUE, so a construction that silently degraded would fail
    // here rather than pass on exit 0.
    const std::string program =
        "module M; import System.Threading; "
        "class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"abc\"); "
        "  using var a := new Atomic<int>(7); "
        "  using var arr := new Array<int>(); "
        "  arr.Add(5); "
        "  using var m := new Mutex(); "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"k\"] := 9; "
        "  var dt := new DateTime(2024, 1, 2); "
        "  return sb.ToString().Length + a.Value + arr[0] + d[\"k\"] + dt.Day; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(26, r.exitCode);   // 3 + 7 + 5 + 9 + 2
}

BEZEL_TEST(builtin_static_class_new_2974,
           a_user_class_sharing_a_builtin_static_name_still_constructs_spec_11_3) {
    // A user class may legitimately be named `Path`. It carries a real `decl`,
    // so the gate — which requires a declaration-less registry built-in — must
    // not claim it.
    const std::string program =
        "module M; "
        "class Path "
        "  public readonly property Depth: int := 3; "
        "end Path; "
        "class C public static method Main(): int "
        "  using var p := new Path(); "
        "  return p.Depth; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
}

BEZEL_TEST(builtin_static_class_new_2974,
           static_call_surfaces_on_those_classes_are_untouched) {
    // The eight rejected names are all still usable the way the stdlib declares
    // them — as static-call receivers. Oracle is the VALUE.
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var joined := Path.Combine(\"a\", \"b\"); "
        "  var bits := BitConverter.DoubleToInt64Bits(0.0); "
        "  Debug.WriteLine(\"alive\"); "
        "  return joined.Length + int(bits); "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);   // "a/b".Length == 3, DoubleToInt64Bits(0.0) == 0
}
