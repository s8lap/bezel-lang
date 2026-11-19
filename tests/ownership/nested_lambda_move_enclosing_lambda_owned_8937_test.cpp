// Ownership — a NESTED lambda may `move` an owned local of its ENCLOSING
// LAMBDA; the byte-identical method spelling (the same `move x` where `x` is
// a local of the METHOD body) is rejected. Issue #8937.
//
// SPEC: §28.3 "Local variables, ownership, and borrows", *Captured owned
// bindings* (v0.359):
//   "Inside a closure, a captured owned binding — an owned local or `owned`
//    parameter of the enclosing body — is uniformly in **borrow state**:
//    reads borrow (the producers rows above), and assignment to it, **a
//    marked transfer from it**, an explicit `Dispose()` or discard of it, and
//    every other owned-consuming use of it are compile errors — a closure
//    cannot consume, reassign, or dispose a captured owned binding."
// The "enclosing body" of a NESTED closure is the ENCLOSING LAMBDA's body,
// so a `move` out of an enclosing lambda's owned local is the same rule the
// method spelling draws.
//
// ROOT CAUSE: `MethodOwnership::scanCapturedOwnedConsumeExpr`'s `UnaryExpr`
// arm rejected a closure-side `move r` only when `isCapturedOwnedBinding(r)`
// held, and that predicate resolves the binding against the METHOD frame's
// `states_`. A local declared in an enclosing LAMBDA's body has no entry in
// any method-frame state map, so the predicate was false and the nested
// closure's `move` of it was admitted.
//
// MEASURED PRE-FIX: the program below compiled rc=0 with no diagnostic, and
// the generated program SEGV'd under `--sanitize=address` (the second call
// of `g` consumes an already-moved-from slot).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// The METHOD twin's exact diagnostic — the lambda form must draw the same
// text, so this needle is the whole claim, not a prefix of it.
const char* kCapturedTransfer =
    "cannot transfer captured owned binding `x` inside a closure — a captured "
    "owned binding is borrow-state throughout the closure (§28.3, v0.184)";

bool hasCapturedTransfer(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kCapturedTransfer) != std::string::npos)
            return true;
    return false;
}

const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "end Sink; ";

// The enclosing lambda declares `x`; the NESTED lambda `g` moves it.
std::string nestedMoveProgram() {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int "
           "    var f := lambda(): int => do "
           "      var x := new Res(1); "
           "      var g := lambda(): int => Sink.Take(move x); "
           "      var a := g(); var b := g(); return a + b; "
           "    end; "
           "    return f(); "
           "  end Main; "
           "end C;";
}

}  // namespace

BEZEL_TEST(nested_lambda_move_enclosing_lambda_owned_8937,
           nested_lambda_move_of_enclosing_lambda_owned_local_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(nestedMoveProgram());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasCapturedTransfer(r));
}

BEZEL_TEST(nested_lambda_move_enclosing_lambda_owned_8937,
           aot_nested_lambda_move_is_a_compile_error_28_3) {
    JitResult jr = runAot(nestedMoveProgram());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
}
