// Codegen / leak — a closure LOCAL passed to a callee that BINDS its
// call-bounded (§28.14) formal to a local was never released, in the
// SPELLINGS the alias proof did not admit. Issue #8983.
//
// Spec: §28.14 (scoped callback parameters — "The call-bounded default":
// "Within the callee the callback may be invoked, BOUND TO LOCALS, and
// passed on as an argument to another call-bounded parameter"), §28.9
// (runtime-managed types), §27.5 (closures), §21.1 (runtime `const`).
// Spec revision v0.359.
//
// THE DEFECT. A closure local's discharge is its own SCOPE-END release,
// armed when the closure escape walk decides the local does not escape. A
// call ARGUMENT is not a read the callee merely looks at, so the walk
// classified the local as an escape — unless the argument position is a
// §28.14 CALL-BOUNDED parameter, where the callee provably holds no
// reference once the call returns. That relief proves the obligation with
// the CALLEE'S BODY (the fail-closed body walk,
// `calleeBodyUsesFormalOnlyByInvocationInspectionOrForwarding`), and the
// walk admitted only ONE spelling of the binding — a plain `var` alias
// bound from the formal and forwarded at a call-bounded position (the
// sibling fix #8972). The other spellings §28.14 admits were DECLINED, so
// the relief was declined, the scope-end release was never armed, and the
// closure cell + its env + its capture cell leaked on every call.
//
// MEASURED on this tree's base (r30-c1 @ 1d2763251), each of the four
// spellings below, compiled with `bezelc --sanitize=address`:
//
//   the `using var` alias binding      15200 byte(s) leaked in 600 allocation(s)
//   the `const` alias binding          15200 byte(s) leaked in 600 allocation(s)
//   the alias of an alias              15200 byte(s) leaked in 600 allocation(s)
//   the INVOKED alias                  15200 byte(s) leaked in 600 allocation(s)
//
// (200 iterations; the direct spelling `C.Id(k)` of the same program is
// clean, and so is the `var` alias spelling the sibling fix #8972 admits.)
//
// THE FIX. The body walk now admits the binding in every spelling §28.14
// admits, and admits the alias's INVOCATION — §28.14 lists the invocation
// FIRST among the admitted uses, so an invocation through an alias is the
// invocation through the formal:
//
//   * the `using var` / `await using var` spelling — §28.9 gives a
//     function-typed local no disposal obligation (its scope-end disposal is
//     a no-op) and the release scan's borrow arm leaves that spelling out
//     (a borrow-initialized function-typed local's slot owns nothing at
//     bind), so the alias holds no reference of its own;
//   * the `const` spelling (a different AST node, `ast::LocalConstDecl`) —
//     §21.1's `const` is not an assignable location, so the alias's purity
//     holds by construction;
//   * a SECOND-LEVEL binding (`var j := h;`) — §28.14's "bound to locals"
//     is not limited to one level;
//   * an INVOCATION of the alias (`h();`).
//
// Every use that is NOT an invocation or a forward at a call-bounded
// position still fails the walk closed (a bare read, an assignment, a
// store, a return, a capture, a re-binding of the alias's name, a binding
// inside a lambda), so a declined shape keeps leaking — a leak, never an
// early release.
//
// ORACLE. `runAotChecked` links and runs the generated code under ASan/LSan,
// so a leak, double-free or use-after-free flips `r.ran` to false; the
// stdout assertion independently pins the computed value, so a premature
// free (a double release) would corrupt it too. The 200-iteration loop is
// the shape the issue was filed with and satisfies the "loop it 50–500x"
// discipline: one execution's cell can hide under the sanitizer's noise.
//
// FAIL-WITHOUT / PASS-WITH. Every row below leaks on the base (each row
// reports the sanitizer report, `r.ran` false) and is LSan-clean with the
// fix.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The callee under test: `F1` binds its call-bounded formal `g` to a local
// in the SPELLING under test, and `Id` only INVOKES the callback it is
// given. Nothing in the chain holds the callback past its call — the alias
// is a local of `F1`'s own frame, which is gone when `F1` returns — so the
// caller's closure local `k` may be released at the caller's scope end.
//
// The `binding` is the callee-body spelling under test; it is spliced into
// `F1`'s body.
std::string calleeBindingProgram(const std::string& binding,
                                 const std::string& use) {
    return "module M; "
           "import System; "
           "class C "
           "  public static method Id(g: () -> int): int "
           "    return g(); "
           "  end Id; "
           "  public static method F1(g: () -> int): int "
           "    " + binding +
           "    " + use +
           "  end F1; "
           "  public static method Run(): int "
           "    var m := 3; "
           "    var k := lambda() => m; "
           "    return C.F1(k); "
           "  end Run; "
           "  public static method Main(): int "
           "    var s := 0; "
           "    for var i in 0..<200 do "
           "      s := s + Run(); "
           "    end for; "
           "    Console.WriteLine(s); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// Run the program under generated-code ASan/LSan and assert the three
// observables: the program RAN (a leak or a memory error clears `ran`), its
// exit value, and its stdout. The stdout pins the computed value, so a
// premature free (a double release) would corrupt it too.
void expectClean(const std::string& source) {
    auto r = runAotChecked(source);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
}

}  // namespace

// Row 1 — the `using var` spelling of the binding. §28.9: a function-typed
// local carries no disposal obligation and a `using var` local of function
// type is disposed at scope end as a no-op, so the alias holds no reference
// of its own; the callee keeps nothing past its call.
//
// A fix that admitted the binding but not the FORWARD of the alias (leaving
// it a retention hazard) would leak here: this is the row that catches it.
BEZEL_TEST(alias_call_bounded_spellings_release_8983,
           using_var_alias_binding_forwarded_releases_the_callers_closure_local) {
    expectClean(calleeBindingProgram("using var h := g;", "return C.Id(h);"));
}

// Row 2 — the `const` spelling of the binding (a different AST node:
// `ast::LocalConstDecl`). §21.1: a `const` binding is not an assignable
// location, so the alias's "bound once, never reassigned" purity holds by
// construction; §28.14 admits the binding whatever the local's spelling.
//
// A fix that widened the `var` spelling only would leak here.
BEZEL_TEST(alias_call_bounded_spellings_release_8983,
           const_alias_binding_forwarded_releases_the_callers_closure_local) {
    expectClean(calleeBindingProgram("const h := g;", "return C.Id(h);"));
}

// Row 3 — a SECOND-LEVEL binding: the alias is bound from the alias.
// §28.14's "bound to locals" is not limited to one level, and the second
// alias's purity is the same (bound once, never reassigned, forwarded at a
// call-bounded position).
//
// A fix that admitted ONE level of binding only would leak here.
BEZEL_TEST(alias_call_bounded_spellings_release_8983,
           alias_of_an_alias_forwarded_releases_the_callers_closure_local) {
    expectClean(calleeBindingProgram("var h := g; var j := h;",
                                     "return C.Id(j);"));
}

// Row 4 — the alias is INVOKED rather than forwarded. §28.14 lists the
// invocation FIRST among the admitted uses ("the callback may be invoked,
// bound to locals, and passed on as an argument to another call-bounded
// parameter"), so an invocation through an alias is the invocation through
// the formal: the callee holds the callback only for the duration of its
// call.
//
// A fix that admitted the binding but not the INVOCATION of the alias would
// leak here: this is the row that catches it.
BEZEL_TEST(alias_call_bounded_spellings_release_8983,
           invoked_alias_releases_the_callers_closure_local) {
    expectClean(calleeBindingProgram("var h := g;", "return h();"));
}
