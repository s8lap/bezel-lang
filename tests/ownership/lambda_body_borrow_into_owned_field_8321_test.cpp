// Ownership — issue #8321: a BORROW stored into an `owned` field from inside
// a LAMBDA body is rejected, exactly as the byte-identical method-level
// spelling has been.
//
// SPEC (v0.359).
//   §28.3.4 *Producers*, the callee's side: "Parameter `p` (no modifier) |
//     Borrowed (source: caller)" — a plain parameter is a BORROW the callee
//     does not own; the caller keeps the ownership.
//   §28.5 / §28.8: an `owned` field is an owned storage location and may
//     only receive an OWNED value; a borrow stored into it is the
//     double-dispose hazard (the field's owner disposes the value when the
//     object is disposed, and the borrow's real owner disposes it too).
//     The compiler's own diagnostic for the method-level spelling says so
//     verbatim: "a borrow cannot be stored into an `owned` field … a
//     double-free (§28.3 / §28.5 / §28.8)".
//   §28.3.4 *borrowing positions*: "a non-`owned` argument" — "`x` retains
//     ownership" — so the callee cannot take the caller's value.
//
// THE HOLE. The ownership pass's statement walk never enters a lambda body
// (a lambda body lives inside a `LambdaExpr`, an expression), so the
// `handleAssignment` arm that applies the owned-field rule to METHOD-body
// statements was unreachable from inside a closure. The lambda-body
// pre-pass (`scanLambdaBodyBorrowStores`, the #3565 driver) drove the
// borrow-store channel but not the owned-field one.
//
// MEASURED ON THE BASE (r30-c1 @ b80788db5): the program below compiled
// clean (rc=0, NO diagnostic) and handed the CALLER's array to the owned
// field, whose dispose freed it. The method twin — the same store in a
// method body — is rejected ("a borrow cannot be stored into an `owned`
// field"), so the two callee kinds disagreed. The hole was benign only
// while the caller's scope-end free did not run; issue #8320's fix (a call
// through a FUNCTION VALUE leaves the caller's owned collection local owned,
// so its scope-end free DOES run) turned it into a hard double free —
// "ERROR: AddressSanitizer: attempting double-free", the first free the
// caller's scope-end `bz_array_free`, the second the field's `DisposeCore`.
//
// THE ORACLE. The row compiles the program and asserts the ERROR
// diagnostic, by needle. On the base the program compiles and no
// diagnostic is produced, so the row is red there and green here.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The shared prelude: a class with an `owned` field, and the lambda that
// stores its plain parameter into it.
constexpr const char* kC =
    "module M; "
    "class C "
    "  private owned saved: Array<int>; "
    "  public constructor() "
    "    this.saved := new Array<int>(); "
    "  end constructor; "
    "  public method Go(): int "
    "    var xs: Array<int> := [1, 2, 3]; "
    "    var f := lambda(l: Array<int>): int => "
    "      do this.saved := l; return l.Count; end; "
    "    var n := f(xs); "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Go; "
    "  public static method Main(): int "
    "    var c := new C(); "
    "    var n := c.Go(); "
    "    Console.WriteLine(n); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// The program is REJECTED with an error naming the rule.
bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// §28.5 / §28.8 (v0.359) — the lambda-body spelling is rejected.
// ===========================================================================

// The lambda's `l` is a BORROW of the caller's `xs` (§28.3.4: "Parameter `p`
// (no modifier) | Borrowed (source: caller)"); storing it into the `owned`
// field would make the field's owner and the borrow's real owner both
// dispose the same array. Before the fix this program compiled clean.
BEZEL_TEST(ownership_lambda_body_borrow_into_owned_field_8321,
            LambdaBodyBorrowStoredIntoOwnedFieldIsRejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        std::string(kC), "a borrow cannot be stored into an `owned` field"));
}
