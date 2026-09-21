// Regression pin for issue #2781 — a METHOD-GROUP reference on an
// Object-typed receiver (`var f := o.GetHashCode;` / `var f := o.Equals;` /
// `var f := o.ToString;` where `o: Object`). spec §8.1 makes
// ToString/Equals/GetHashCode universal Object members occupying FIXED vtable
// slots (3/4/5, after the Class header — not the user-virtuals
// `vtableSlot_` map that `makeMethodThunk` looks up), and §27.6 makes bare
// references to them `() -> T` / `(Object) -> bool` function values. The
// path that builds a method-group thunk for these three names is broken in
// two distinct ways.
//
// THE FIRST DEFECT (silent wrong value for GetHashCode). `makeMethodThunk`
// reads `vtableSlot_[ms]` to pick the vtable offset; the universal Object
// members are NOT registered there (only user-declared virtuals are), so the
// `ms->decl == nullptr` check (or its absence) led the thunk to dispatch
// through a zero slot — a memory location the LLVM verifier accepted (slot
// 0 of a vtable is normally the type-info pointer, not a function pointer)
// and which then returned whatever was on the stack, which `lowerCall`
// accepted as an int and stored in the local. The reported repro returns
// `thunk=0 direct=1984007414`, exit 3, NO crash, NO ASan report — silent.
//
// THE SECOND DEFECT (sema rejection for ToString). `resolveMemberAccess`
// handled `o.ToString` by the CG27c primitive-intrinsic path
// (ExprTypecheck_Calls.cpp ~16070), which unconditionally types the
// MemberAccessExpr as `string` for every primitive receiver including
// Object. The enclosing LocalDecl's type inference then sets the binding
// local's declType to `string`, and the §27.7 visitCallExpr arm rejects
// `f()` ("cannot call a value of type `string`"). Equals/GetHashCode on
// Object are not in that allow-list, so sema left their MemberAccessExpr
// exprType null and the LocalDecl's declType inherited that null — the
// shape the §27.7 arm skips, deferring to codegen where the thunk was
// produced but the LocalDecl's slot remained un-typed (codegen's
// #2434 zero-init fallback silently returned 0 instead of the hash — the
// same silent-wrong-value as the first defect, just through a different
// site). On `o.ToString()` (a CALL, not a bare reference) the §8.5 path
// types and lowers correctly; the asymmetry between the call and the bare
// reference is the issue's core.
//
// THE FIX has two halves, mirroring the two defects:
//   1. A new `makeObjectMemberThunk` (LlvmBackend_Layout.cpp) that emits a
//      thunk with the universal member's ABI: null receiver → default
//      (0/false/""), string receiver → content semantics (with retain for
//      ToString), class/box receiver → vtable[fixed slot] dispatch.
//   2. A CG60 trigger (LlvmBackend_Exprs.cpp ~6280) that detects the
//      Object-typed receiver case, builds the matching Function TypeRef,
//      and routes to `makeObjectMemberThunk`. A paired sema change
//      (ExprTypecheck_Calls.cpp ~16069) narrows CG27c so it does not
//      pre-type `o.ToString` when the access is a BARE REFERENCE (the
//      call form keeps the existing §8.5 typing), letting the codegen
//      half patch the LocalSymbol's declType to Function.
//
// THE ORACLE. The plain oracle catches the silent-wrong-value half (every
// shape returns its §8.1-correct value, so the program returns the
// expected exit code rather than the wrong-branch sentinel 3). The ASan
// oracle catches the same half under the more sensitive allocator; the
// bug is not a memory error so the two oracles carry the same evidence
// for the success path. Exit codes are small (< 256) to fit runAot's
// WEXITSTATUS-masked byte.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

// fix #4578 (§8.1 / §27.6 / §28.9, v0.306) RETIRED the leak-tolerant helper
// this file used to carry. Its comment claimed the Equals/ToString thunks'
// receiver env_ptr was "a borrow that LSan cannot prove live" — a conservative
// false positive — and that "the GetHashCode variant happens to be allocated
// in a shape LSan accepts, so it uses runChecked directly".
//
// BOTH halves of that were wrong. The leak was REAL: `makeObjectMemberThunk`
// mints a +1 closure cell (48 B + an 8 B env) and the emitted IR carried ZERO
// `bz_closure_release`, because the #2781 arm of `isPlusOneClosureProducer`
// gated on an `exprType` that CG60 itself writes at LOWERING time, so the
// function-entry `collectClosureReleaseEligible` scan never admitted the
// local. All THREE members leaked identically; LSan merely happened to still
// find a stale pointer to the GetHashCode cell at leak-check time. That luck
// is not an oracle, and it ran out. With the cell now released on every edge,
// every ASan row below uses the STRICT `runChecked` — ASan and LeakSanitizer
// both on.

// §8.3 — Object.GetHashCode on a string receiver must equal the direct
// call's hash (the issue's headline case: it returned 0 instead of the
// hash, exit 3 instead of 21).
constexpr const char* kGetHashCodeString =
    "module M; "
    "class C public static method Main(): int "
    "  var s := \"objhash\"; "
    "  var o: Object := s; "
    "  var f := o.GetHashCode; "
    "  var h := f(); "
    "  var d := s.GetHashCode(); "
    "  if h = d then return 21; end; "
    "  return 3; "
    "end Main; end C;";

// §8.3 — Object.Equals(Object) on a string receiver must follow §9.6
// content equality (the same spelling the issue lists next to ToString).
constexpr const char* kEqualsString =
    "module M; "
    "class C public static method Main(): int "
    "  var s := \"objeq\"; "
    "  var o: Object := s; "
    "  var f := o.Equals; "
    "  if f(\"objeq\") then return 23; end; "
    "  return 3; "
    "end Main; end C;";

// §8.5 — Object.ToString on a string receiver must return the receiver
// (the issue's third case; rejected by §27.7 before the fix).
constexpr const char* kToStringString =
    "module M; "
    "class C public static method Main(): int "
    "  var s := \"objvirt\"; "
    "  var o: Object := s; "
    "  var f := o.ToString; "
    "  var t := f(); "
    "  if t = \"objvirt\" then return 19; end; "
    "  return 3; "
    "end Main; end C;";

// CONTROL — the direct call forms keep working (the CG60 codepath does
// not intercept `o.M()` calls). Pinned so a regression that pushes the
// method-group fix into the call path is caught.
constexpr const char* kDirectCalls =
    "module M; "
    "class C public static method Main(): int "
    "  var s := \"objdirect\"; "
    "  var o: Object := s; "
    "  var n := 0; "
    "  if o.Equals(\"objdirect\") then n := n + 1; end; "
    "  if o.ToString() = \"objdirect\" then n := n + 2; end; "
    "  if o.GetHashCode() = s.GetHashCode() then n := n + 4; end; "
    "  return n; "
    "end Main; end C;";

// fix #4578 (§8.1 / §27.6 / §28.9, v0.306) — LOOP-AMPLIFIED leak pin for all
// THREE universal members. A one-shot leak repro of this shape is a VACUOUS
// oracle: LSan reports only *unreachable* allocations, and a single never-
// released cell can still be found from a dead stack slot or a register at
// leak-check time — which is exactly how the GetHashCode row above passed the
// strict oracle for revisions while the cell was never released at all.
// 500 iterations x 3 members removes the luck: pre-fix this leaks 83,944 B in
// 2,998 allocations, post-fix it is clean. `ok` counts 3 successes per
// iteration (1500) and the exit code stays under runAot's WEXITSTATUS byte.
constexpr const char* kMethodGroupLoop =
    "module M; "
    "class C public static method Main(): int "
    "  var s := \"objloop\"; "
    "  var o: Object := s; "
    "  var d := s.GetHashCode(); "
    "  var ok := 0; "
    "  var i := 0; "
    "  while i < 500 do "
    "    var f := o.GetHashCode; "
    "    var g := o.Equals; "
    "    var t := o.ToString; "
    "    if f() = d then ok := ok + 1; end; "
    "    if g(\"objloop\") then ok := ok + 1; end; "
    "    if t() = \"objloop\" then ok := ok + 1; end; "
    "    i := i + 1; "
    "  end; "
    "  if ok = 1500 then return 29; end; "
    "  return 3; "
    "end Main; end C;";

}  // namespace

// §8.3 — GetHashCode method group on an Object-typed receiver, plain
// oracle. Pre-fix this returned 3.
BEZEL_TEST(method_group_object_receiver_2781, get_hash_code_method_group_string) {
    BEZEL_EXPECT_EQ(runPlain(kGetHashCodeString), 21);
}

// §8.3 — same under ASan. Pre-fix this returned 3 (silent, no ASan
// report) because the wrong slot's load yielded 0; ASan does not catch
// the wrong-slot dispatch because the vtable slot 0 read is in-bounds,
// just the wrong data.
BEZEL_TEST(method_group_object_receiver_2781, get_hash_code_method_group_string_asan) {
    BEZEL_EXPECT_EQ(runChecked(kGetHashCodeString), 21);
}

// §8.3 — Equals method group on an Object-typed receiver. Pre-fix this
// also returned 3 (same wrong-slot defect as GetHashCode).
BEZEL_TEST(method_group_object_receiver_2781, equals_method_group_string) {
    BEZEL_EXPECT_EQ(runPlain(kEqualsString), 23);
}

// §8.3 / §28.9 — same under ASan, STRENGTHENED by fix #4578 (v0.306). This
// row used to run on the leak-tolerant oracle, on the premise that the
// closure cell's env_ptr is "a borrow LSan cannot prove live". That premise
// was wrong: the cell itself was never released at all (zero
// `bz_closure_release` in the emitted IR), so LSan's report was a TRUE
// positive. With the scope-end release armed, the strict oracle holds.
BEZEL_TEST(method_group_object_receiver_2781, equals_method_group_string_asan) {
    BEZEL_EXPECT_EQ(runChecked(kEqualsString), 23);
}

// §8.5 — ToString method group on an Object-typed receiver. Pre-fix this
// was REJECTED by the compiler ("cannot call a value of type `string`");
// no executable was produced.
BEZEL_TEST(method_group_object_receiver_2781, to_string_method_group_string) {
    BEZEL_EXPECT_EQ(runPlain(kToStringString), 19);
}

// §8.5 / §28.9 — same under ASan, STRENGTHENED by fix #4578 (v0.306) for the
// reason spelled out on the Equals row above.
BEZEL_TEST(method_group_object_receiver_2781, to_string_method_group_string_asan) {
    BEZEL_EXPECT_EQ(runChecked(kToStringString), 19);
}

// CONTROL — direct calls on the same three universal members must keep
// working. Catches a regression that pushes the method-group path into
// the call path or breaks the §8.5 ToString typing that the call form
// relies on. 1 + 2 + 4 = 7.
BEZEL_TEST(method_group_object_receiver_2781, direct_calls_on_object_still_work) {
    BEZEL_EXPECT_EQ(runPlain(kDirectCalls), 7);
}

// fix #4578 (§8.1 / §27.6 / §28.9, v0.306) — the DETERMINISTIC pin for the
// missing scope-end `bz_closure_release` on the cell `makeObjectMemberThunk`
// mints. `isObjectMemberMethodRef` gated on an `exprType` that CG60 itself
// writes at lowering time, so the function-entry `collectClosureReleaseEligible`
// scan never admitted the local and no release was ever armed. FAIL-WITHOUT
// measured on the pre-fix compiler: exit 1, "83944 byte(s) leaked in 2998
// allocation(s)"; with the fix, exit 29 and LSan-clean. The plain-oracle
// value (1500 successful agreements) also pins that releasing the cell does
// NOT disturb the receiver borrow the env holds — a double-release would
// fault under ASan here rather than leak.
BEZEL_TEST(method_group_object_receiver_2781,
           method_group_loop_releases_every_cell_spec_8_1) {
    BEZEL_EXPECT_EQ(runChecked(kMethodGroupLoop), 29);
}
