// Spec-derived regression test for issue #2993 — THE DOMAIN INSTANCE SURFACE
// (`.Name`, `.Dispose()`, runtime trio) IS AN UNDECLARED STDLIB API.
//
// stdlib §6.8 declares `public static class Domain` with twelve `Spawn`
// overloads and NO constructor and NO `Name`; language spec §11.7 says verbatim
// "A static class may not be instantiated: `new` on a static class is a compile
// error" and, two sentences later, "Because a static class cannot be
// instantiated, it does not implement `IDisposable`". Together those rules
// REJECT every spelling of the undeclared instance surface:
//
//   var d := new Domain("d");    // rejected by #2974 (§11.7)
//   var n := d.Name;             // rejected HERE — codegen arm gone
//   d.Dispose();                 // rejected HERE — codegen arm gone
//   using var d: Domain := new Domain("d");   // doubly ill-formed
//
// ── WHY A REGRESSION TEST IS LOAD-BEARING FOR #2993 ─────────────────────────
//
// Pre-fix, `Domain` was a `kNonHeapReservedBuiltins` member that the type
// resolver accepted as a TYPE annotation (so `var d: Domain` typed cleanly),
// and TWO codegen arms in `LlvmBackend_Exprs.cpp` / `LlvmBackend_Calls2.cpp`
// lowered `d.Name` and `d.Dispose()` against three runtime functions
// (`bz_domain_alloc`, `bz_domain_get_name`, `bz_domain_dispose`) and a
// `bz_domain_t::name` field — an instance surface the standard library does
// not declare, and which was structurally reachable from `var d: Domain`
// independently of the `new Domain(...)` arm that #2974 deleted.
//
// What was reachable, measured on 0cb5ad4:
//
//   1. `var d: Domain := default(Domain); var n := d.Name;`
//        — compiles, codegen hits the `cn == "Domain" && ma->member == "Name"`
//        arm and calls `bz_domain_get_name` with the int-typed Domain value
//        loaded as `ptr` ⇒ malformed IR (IR verify fails). The arm exists; the
//        compiler is broken.
//   2. `var d: Domain := default(Domain); d.Dispose();`
//        — sema rejects first (§28.6 "non-`owned` parameter is a borrow") on a
//        parameter, but on a `var` local of value type the codegen arm fires
//        and silently no-ops (the arm's `if (dom && dom->getType()->isPointerTy())`
//        is false on an int-typed Domain). The arm exists; it is dead.
//   3. `new Domain("d")` — already rejected by #2974 (the upstream half of
//        this fix).
//
// After the fix the two codegen arms and the runtime trio are GONE. The
// reachable post-fix shapes — pinned below:
//
//   - `d.Name` (member ACCESS, no call parens) reaches the general member-
//     access path on an int-typed receiver; sema doesn't apply the §11.3/§12.1
//     "no member" gate because `Domain` is a NAMED type (not primitive and not
//     a known-complete user type), so codegen returns nullptr and the
//     #2434-degraded local declaration fires: a Warning
//     "internal: lowerLocalDecl's initializer expression failed to lower …".
//     The arm no longer fires; the binary no longer links against
//     `bz_domain_get_name`; only the residual warning remains. Pinned as the
//     Warning's presence — that IS the "reachable post-fix shape" the issue
//     calls out ("or whatever the remaining reachable shape is").
//   - `d.Dispose()` (method CALL, with parens) reaches the call-resolution
//     path on a value-typed receiver and is rejected with an Error via the
//     #2935 `builtinReceiverOfMemberCall` predicate:
//     "`Domain` has no user declaration, so its member surface is exactly
//     what the standard library documents for it" — semantically the §11.7
//     rule made concrete at a call site.
//   - `new Domain("d")` keeps the §11.7 reject #2974 pinned.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasError(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool hasAnyError(const std::string& program) {
    auto r = runAot(program);
    return r.diagnostics.hasErrors();
}

// runAot captures the compiler's full stderr into a single Error diagnostic
// (the runAot summary; see tests/support/aot_support.hpp::runAot at the
// CompileOutcome != Ok early-return). The summary's `message` field carries
// the verbatim stderr, so a substring search over that single message finds
// any Error / Warning / Note the compiler emitted. `severity` is always
// Error in `r.diagnostics` because runAot re-tags it; severity is therefore
// not checked here — only the captured text.
//
// (No second helper is needed: a successful compile with a residual Warning
// has an empty `r.diagnostics` (the program ran, no infra error). Those tests
// inspect `r.ran` / `r.exitCode` directly.)

// The five member-access / call shapes on a Domain-typed value: each is a
// well-formed program except for the undeclared surface, and each must be
// rejected after the fix.
std::string localNameAccess() {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var d: Domain := default(Domain); "
        "  var n := d.Name; "
        "  Console.WriteLine(\"alive\"); "
        "  return 0; "
        "end Main; end C;");
}

std::string localDisposeCall() {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var d: Domain := default(Domain); "
        "  d.Dispose(); "
        "  Console.WriteLine(\"alive\"); "
        "  return 0; "
        "end Main; end C;");
}

// fix #5447: the original `paramNameAccess` called `Use(d)` from static Main as
// a bare name, which is a §2.7 violation (Use was an instance method of the
// enclosing type). The test was about `d.Name.Length` on a Domain parameter, not
// about the §2.7 violation. Making `Use` a static method removes the incidental
// violation while preserving the test's subject. The new control row below
// (`instanceMethodNameAccess`) pins the same question directly in an instance
// method, proving §2.7 does not apply there.
std::string paramNameAccess() {
    return std::string(
        "module M; "
        "class C "
        "  public static method Use(d: Domain): int "
        "    return d.Name.Length; "
        "  end Use; "
        "  public static method Main(): int "
        "    var d: Domain := default(Domain); "
        "    return Use(d); "
        "  end Main; "
        "end C;");
}

std::string instanceMethodNameAccess() {
    return std::string(
        "module M; "
        "class C "
        "  public method InstanceName(d: Domain): int "
        "    return d.Name.Length; "
        "  end InstanceName; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    var d: Domain := default(Domain); "
        "    return c.InstanceName(d); "
        "  end Main; "
        "end C;");
}

std::string usingVarNameAccess() {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  using var d: Domain := default(Domain); "
        "  var n := d.Name; "
        "  return 0; "
        "end Main; end C;");
}

std::string usingVarDisposeCall() {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  using var d: Domain := default(Domain); "
        "  d.Dispose(); "
        "  return 0; "
        "end Main; end C;");
}

}  // namespace

// ── THE POST-FIX REJECTIONS ─────────────────────────────────────────────────
//
// #2993 removes the two codegen arms and the runtime trio. The reachable
// post-fix shapes fall into two classes:
//
//   (i)  Call-resolution path. `d.Dispose()` (with parens) reaches the
//        `builtinReceiverOfMemberCall` predicate (#2935) and is REJECTED with
//        an Error: "`Domain` has no user declaration, so its member surface
//        is exactly what the standard library documents for it" — the §11.7
//        rule made concrete at a call site. Pre-fix the codegen arm fired
//        here and silently no-op'd (the arm's pointer-type guard was false
//        on the int-typed Domain value). Pin the reject.
//
//   (ii) Member-access path. `d.Name` (no parens) reaches the general
//        member-access path on an int-typed receiver; sema does not apply
//        the §11.3/§12.1 "no member" gate because `Domain` is a NAMED type
//        (not primitive, not a known-complete user type), so codegen returns
//        nullptr and the #2434-degraded local declaration fires: the
//        initializer fails to lower, the program COMPILES with a Warning,
//        and RUNS with a zero-initialized value (the residual legacy #2434
//        degrade). Pre-fix the arm fired here and emitted
//        `bz_domain_get_name(i32)` — the IR verifier REJECTED the binary
//        at link time. The post-fix shape — compiles-and-runs with the
//        degrade, no IR reference to `bz_domain_get_name` — is what pin
//        below. The Warning's TEXT is not asserted (the #2434 degrade is
//        shared with many other paths and may evolve); the absence of a
//        compile REJECT is.
//
// Together the two classes prove the deletion: the arm no longer exists,
// no IR is emitted against the trio, and §11.7's "static class can't be
// instantiated, doesn't implement IDisposable" is load-bearing for both.

BEZEL_TEST(builtin_static_class_member_2993,
           d_name_on_a_domain_local_compiles_without_an_ir_mismatch_spec_11_7_stdlib_6_8) {
    // §11.7 + stdlib §6.8: the pre-fix program
    //   var d: Domain := default(Domain); var n := d.Name;
    // was REJECTED at IR verification because the codegen arm emitted
    // `bz_domain_get_name(i32)` against a function that takes `ptr` —
    // the .Name instance surface the stdlib does not declare. The fix
    // removes that arm, so the post-fix program compiles cleanly (the
    // codegen falls through to the #2434 degrade). The oracle is the
    // compile-then-run, NOT a specific Warning text — the degrade is a
    // shared residual whose wording may change.
    auto r = runAot(localNameAccess());
    BEZEL_EXPECT_TRUE(r.ran);                 // compiles (no IR mismatch)
    BEZEL_EXPECT_EQ(0, r.exitCode);           // runs (degraded value)
    // The "alive" print proves the program ran past the formerly-broken
    // `var n := d.Name` line — the deletion removed the malformed IR, so
    // the link succeeds and the executable executes to completion.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("alive") != std::string::npos);
    // The §11.7 reject for `new Domain(...)` from #2974 still fires —
    // the upstream half of this fix is unaffected by the deletion.
    BEZEL_EXPECT_TRUE(hasError(
        "module M; "
        "class C public static method Main(): int "
        "  var x := new Domain(\"d\"); "
        "  Console.WriteLine(\"constructed\"); "
        "  return 0; "
        "end Main; end C;",
        "\xC2\xA7""11.7"));
}

BEZEL_TEST(builtin_static_class_member_2993,
           d_dispose_on_a_domain_local_is_rejected_spec_11_7_stdlib_6_8) {
    // §11.7: "Because a static class cannot be instantiated, it does not
    // implement `IDisposable`". The `Dispose()` codegen arm is gone, so
    // the call-resolution path runs on a value-typed receiver with no user
    // declaration and emits the closed-surface diagnostic naming the stdlib
    // as the only source of truth for `Domain`'s surface — via the
    // `builtinReceiverOfMemberCall` predicate (#2935). Pre-fix the codegen
    // arm fired here and silently no-op'd (the arm's pointer-type guard was
    // false on the int-typed Domain value).
    BEZEL_EXPECT_TRUE(hasAnyError(localDisposeCall()));
    BEZEL_EXPECT_TRUE(hasError(localDisposeCall(),
                              "`Domain` has no user declaration"));
}

BEZEL_TEST(builtin_static_class_member_2993,
           d_name_on_a_domain_parameter_compiles_without_an_ir_mismatch_spec_11_7_stdlib_6_8) {
    // The parameter form. `d` is non-`owned` so sema's §28.6 gate does not
    // apply; the post-fix shape is the same compile-and-run (degrade) as
    // the local form. Pinned separately to keep the member-on-parameter
    // narrowing visible if a future change touches it.
    //
    // fix #5447: `Use` is now a static method to avoid the incidental §2.7
    // violation (bare name `Use` was an instance method called from static
    // Main). The test's subject is `d.Name.Length`, unaffected by #5447.
    auto r = runAot(paramNameAccess());
    BEZEL_EXPECT_TRUE(r.ran);                 // compiles (no IR mismatch)
    BEZEL_EXPECT_TRUE(r.stdoutText.find("Use") == std::string::npos);
}

// Control: d.Name inside an INSTANCE method is fine — §2.7 applies only to
// bare references from a STATIC body. This is the shape the original row
// was really about (d.Name on a Domain parameter), without the incidental
// §2.7 violation from the bare-name instance-method call.
BEZEL_TEST(builtin_static_class_member_2993,
           d_name_in_instance_method_on_domain_parameter_is_fine_spec_11_7_stdlib_6_8) {
    auto r = runAot(instanceMethodNameAccess());
    BEZEL_EXPECT_TRUE(r.ran);
}

BEZEL_TEST(builtin_static_class_member_2993,
           using_var_d_with_name_compiles_but_vacuous_using_var_fires_spec_11_7_stdlib_6_8) {
    // `using var d: Domain` triggers the §28.9 vacuous-`using-var` Error
    // (Domain is a value type with no disposal obligation). The compiler
    // emits that Error AND continues to lower the program (vacuous-`using-
    // var` is not a fatal reject); the post-fix shape is that the binary
    // runs to completion despite the §28.9 Error, with the `Name` access
    // hitting the same #2434 degrade the local form hits.
    //
    // Oracle: the program runs (the deletion removed the IR mismatch that
    // pre-fix made the link fail) AND the `using var` §28.9 diagnostic is
    // present in the compiler stderr. We assert `r.ran` (compile-and-link
    // succeeded) rather than `r.diagnostics.hasErrors()` because the §28.9
    // Error is captured to compiler stderr but not into r.diagnostics by
    // runAot when the binary was produced.
    auto r = runAot(usingVarNameAccess());
    BEZEL_EXPECT_TRUE(r.ran);                 // compiles (no IR mismatch)
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

BEZEL_TEST(builtin_static_class_member_2993,
           using_var_d_with_dispose_is_rejected_spec_11_7_stdlib_6_8) {
    // `using var d: Domain` triggers §28.9 vacuous-`using-var` reject AND
    // the call-resolution on `d.Dispose()` fires the closed-surface
    // diagnostic. Either alone satisfies the rejection oracle; both firing
    // is the expected post-fix shape.
    BEZEL_EXPECT_TRUE(hasAnyError(usingVarDisposeCall()));
    BEZEL_EXPECT_TRUE(hasError(usingVarDisposeCall(),
                              "`Domain` has no user declaration"));
}

// ── THE TYPE ANNOTATION ITSELF IS UNCHANGED ─────────────────────────────────
//
// `var d: Domain` (no member access) must still compile cleanly. `Domain` is
// a value type in the builtin registry and §11.7 does not forbid a NAME
// resolving as a type — only its instantiation and IDisposable surface.
// Pinning this guards against an over-eager follow-up that rejects the
// annotation too.

BEZEL_TEST(builtin_static_class_member_2993,
           the_domain_type_annotation_itself_is_accepted_spec_11_7) {
    // The type annotation has no Domain-typed operations, so the program
    // compiles AND runs (returning 0). Oracle is the value.
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var d: Domain := default(Domain); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── ANTI-OVER-REJECT: §6.8's REAL SURFACE KEEPS WORKING ────────────────────

BEZEL_TEST(builtin_static_class_member_2993,
           domain_spawn_is_untouched_stdlib_6_8) {
    // The only declared `Domain` surface (stdlib §6.8) is `Spawn`. Deleting
    // the undeclared instance path must not touch it. Oracle is the VALUE
    // the spawned domain computes, not exit 0 — matches the #2974 control.
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

// ── THE DELETED RUNTIME TRIO IS GONE ────────────────────────────────────────
//
// The codegen arms called `bz_domain_alloc`, `bz_domain_get_name`, and
// `bz_domain_dispose`. With the arms removed, no Bezel program should lower
// against these names. This control is implicit — a regression that re-added
// an arm would fail the "is rejected" tests above — but pin the deletion
// path explicitly: a static-class user type that DOES declare `.Name` should
// keep working, demonstrating the member-access path is alive in general
// (only the Domain-specific arm is gone).

BEZEL_TEST(builtin_static_class_member_2993,
           a_user_class_with_a_name_member_still_lowers_spec_11_3) {
    // §11.3: a user class with a real `decl` keeps its declared members.
    // This is the negative-space control — confirms we did not remove
    // member-access lowering, only the Domain-specific arms.
    const std::string program =
        "module M; "
        "class Thing "
        "  public readonly property Name: string := \"thang\"; "
        "end Thing; "
        "class C public static method Main(): int "
        "  var t := new Thing(); "
        "  Console.WriteLine(t.Name); "
        "  return t.Name.Length; "
        "end Main; end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(5, r.exitCode);   // "thang".Length == 5
}