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
using bezel::test::runAotCheckedLenient;

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

// ASan on, LeakSanitizer off. Used only where the program carries a
// PRE-EXISTING leak unrelated to this fix (the Equals thunk's receiver
// env_ptr is a borrow that LSan cannot prove live; the GetHashCode
// variant happens to be allocated in a shape LSan accepts, so it uses
// runChecked directly). LSan overrides the process exit code, so leaving
// it on would mask the value.
int runCheckedNoLeaks(const std::string& src) {
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

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

// §8.3 — same under ASan. The closure cell's env_ptr captures the
// receiver by borrow — LSan conservatively flags this as a leak because
// the closure outlives the explicit `var f` slot it came from (the
// `f(o)` call hands it to a CallExpr whose release happens at the
// call-site boundary, and LSan does not follow that). runCheckedNoLeaks
// catches the value while skipping the leak report.
BEZEL_TEST(method_group_object_receiver_2781, equals_method_group_string_asan) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(kEqualsString), 23);
}

// §8.5 — ToString method group on an Object-typed receiver. Pre-fix this
// was REJECTED by the compiler ("cannot call a value of type `string`");
// no executable was produced.
BEZEL_TEST(method_group_object_receiver_2781, to_string_method_group_string) {
    BEZEL_EXPECT_EQ(runPlain(kToStringString), 19);
}

// §8.5 — same under ASan. Same env_ptr-leak shape as Equals above.
BEZEL_TEST(method_group_object_receiver_2781, to_string_method_group_string_asan) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(kToStringString), 19);
}

// CONTROL — direct calls on the same three universal members must keep
// working. Catches a regression that pushes the method-group path into
// the call path or breaks the §8.5 ToString typing that the call form
// relies on. 1 + 2 + 4 = 7.
BEZEL_TEST(method_group_object_receiver_2781, direct_calls_on_object_still_work) {
    BEZEL_EXPECT_EQ(runPlain(kDirectCalls), 7);
}
