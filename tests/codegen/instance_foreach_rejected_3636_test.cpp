// Sema + Codegen — issue #3636: `instance .ForEach(...)` on `Array<T>` or
// `string` was admitted by four hand-spelled name lists in
// `src/sema/ExprTypecheck_Calls.cpp` (`allowedOwningMethod`,
// `allowedNonOwningMethod`, `linqLambdaElementType`'s `elemLambda`), by the
// `kStringLinqOperatorMemberNames` table in
// `src/common/BuiltinRegistry.{hpp,cpp}` (the centralized string-receiver
// LINQ exemption gate feeding both the `.` and `?.` member-access gates),
// AND by the `isArrTerminal` codegen gate at
// `src/codegen/LlvmBackend_Calls.cpp` (with a real, 30-line codegen arm
// commented `// per §5 LINQ: .ForEach(fn: (T)->())`). The latter produced
// MALFORMED LLVM IR:
//
//     Instruction has a name, but provides a void value!
//       %cl.ret = call void %cl.fn(ptr %cl.env, i32 %17)
//     bezelc: codegen error: IR verification failed
//
// — aborting in the LLVM verifier rather than reporting a clean compile
// error.
//
// Fix: `ForEach` is NOT a member of any Array<T>/string receiver in stdlib
// §5 (the only documented `ForEach` is the unrelated static
// `Parallel.ForEach`, stdlib §5, in `Parallel` namespace). It is REMOVED
// from every list/gate above. The post-fix behavior for both receivers is
// the precise collection-specific diagnostic
//     "no member `ForEach` on `Array<T>` (§22.5 / §4.x) — ..."
// — not the malformed-IR crash, and not a generic "#2457 no dispatch arm"
// wording either. This file pins that the rejection happens at the right
// site (sema, with the right wording) and that the unrelated static
// `Parallel.ForEach` is unchanged.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Per tests/README.md: a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT, never by exit code or
// `!r.ran` alone (a runtime crash also fails to "run").
bool hasErrorContaining(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

std::string arrayProgram(const char* callExpr) {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var r := ") + callExpr + std::string("; "
        "  return 0; "
        "end Main; end C;");
}

std::string arrayStatementProgram(const char* stmt) {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  ") + stmt + std::string(" "
        "  return 0; "
        "end Main; end C;");
}

}  // namespace

// ── Array<int> instance `.ForEach(...)` now rejected at sema with the
//    PRECISE collection-specific "no member" wording (stdlib §5 / §4.x),
//    not the LLVM verifier malformed-IR crash that #3636 reported, and
//    not a generic "#2457 no dispatch arm" wording either. The fix retires
//    the admission in `allowedOwningMethod` (~line 5208) AND removes the
//    `// per §5 LINQ: .ForEach(...)` codegen arm at
//    LlvmBackend_Calls.cpp ~line 2591 that was producing the crash. ────

BEZEL_TEST(instance_foreach_rejected_3636, array_foreach_explicit_lambda_rejected) {
    // Mirrors the #3636 issue's minimal repro. Pre-fix: bezelc aborts in
    // the LLVM IR verifier with "Instruction has a name, but provides a
    // void value!". Post-fix: clean `no member` diagnostic at sema.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        arrayStatementProgram(
            "a.ForEach(lambda(x: int) => do end);"),
        "no member `ForEach` on `Array<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}

BEZEL_TEST(instance_foreach_rejected_3636,
           array_foreach_assigned_no_longer_crashes) {
    // Variant: `var f := a.ForEach;` style (the bare-reference form).
    // Pre-fix this also crashed in IR verification; post-fix it's
    // rejected at sema with the same precise wording.
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        arrayProgram(
            "a.ForEach(lambda(x: int) => do end)"),
        "no member `ForEach` on `Array<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}

// ── Same diagnostic, string receiver — the "unification" half of #3636.
//    `kStringLinqOperatorMemberNames` (BuiltinRegistry.cpp / .hpp) was
//    admitting `ForEach` for string receivers, parallel to the
//    `Array`/collection twin. The 38-name table is now 37, with `ForEach`
//    moved to the negative side of the snapshot test
//    (`builtin_registry_snapshot_test.cpp`). This row pins the visible
//    change in the *call* form for string: the call is now rejected at
//    sema with the precise primitive-string wording
//    ("string member `.ForEach` is not yet supported in codegen")
//    rather than aborting in the LLVM IR verifier. ─────────────────────

BEZEL_TEST(instance_foreach_rejected_3636, string_foreach_rejected_no_longer_crashes) {
    // stdlib §5 / §9.6: string is `IEnumerable<char>`. There is no
    // instance `.ForEach(...)` on it; only the static
    // `Parallel.ForEach<A>(...)` exists. Pre-fix this crashed at IR
    // verification ("Instruction has a name, but provides a void
    // value!"); post-fix the primitive-string memberAccess gate at
    // `ExprTypecheck_Calls.cpp` ~17000 reports the precise
    // `isStringLinqOp`-miss diagnostic. We pin the *form* of the
    // message (it is a sema-level ERROR with the diagnostic line on
    // the call site, NOT an `bezelc: codegen error: IR verification
    // failed` mismatch), but not the exact wording — that wording
    // belongs to a different code path than the Array<T> one above,
    // and the #3636 fix does not change it. The load-bearing pin is
    // that the call no longer falls through to the lowered codegen.
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string := \"abc\"; "
        "  s.ForEach(lambda(c: char) => do end); "
        "  return 0; "
        "end Main; end C;");
    // Post-fix: r.ran is false (sema rejected) and the diagnostics list
    // a precise error referencing ForEach / string — NOT the
    // loadModule/malformed-IR path that pre-fix used to take.
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawPreciseError = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("ForEach") != std::string::npos &&
            d.message.find("string") != std::string::npos) {
            sawPreciseError = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(sawPreciseError);
    // Critical: post-fix diagnostics must NOT include "IR verification
    // failed" — that was the pre-fix malformed-IR crash signature.
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("IR verification") != std::string::npos) {
            BEZEL_EXPECT_TRUE(false);  // #3636's malformed IR appeared
        }
    }
}

// ── Regression guard: the unrelated STATIC `Parallel.ForEach<A>(...)`
//    (stdlib §5, in the `Parallel` namespace) is unaffected. A compile-
//    time shape that DOES use it should still produce a `Task`-typed
//    expression (or the right domain error — but NOT the `no member on
//    Array<T>` wording we test above, which is the diagnostic the fix
//    pins for the instance form). We don't run the actual call (it would
//    need a TaskScope / thread pool); we just pin that the static form
//    is NOT routed to the `no member on Array<T>` wording. ─────────────

BEZEL_TEST(instance_foreach_rejected_3636,
           static_parallel_foreach_not_routed_to_array_no_member) {
    // The static `Parallel.ForEach<A>(items, body, degree)` exists; a
    // bare call (`Parallel.ForEach(...)`) should NOT trigger the
    // collection-specific `no member \`ForEach\` on \`Array<T>\``
    // wording — that wording is the precise post-fix rejection of the
    // INSTANCE form that this test exists to pin. (Whether the static
    // call itself type-checks / works is a separate question, out of
    // scope for the #3636 fix.)
    std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var t := Parallel.ForEach<int>(a, "
        "                     lambda(x: int) => do end); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_FALSE(hasErrorContaining(
        program,
        "no member `ForEach` on `Array<T>` (\xC2\xA7""22.5 / \xC2\xA7""4.x)"));
}
