// Sema tests for #1606 — spec §28.14 (v0.180): `scoped` on a STATIC method
// with no `owned` return is a compile error (deferred from #1541).
//
// §28.14: "A `scoped` parameter on a **static** method is admitted only when
// the method's return is `owned`, so that a call site can resolve bounding
// form 2 or 3; declaring `scoped` on a static method with no `owned` return
// is a compile error (v0.180) — there is no receiver, no constructed object,
// and no instance to hold the stored value, so nothing could ever bound the
// storage."
//
// Non-static methods are unaffected: form 5 ("otherwise -> the receiver")
// always resolves a bounding object for an instance method, regardless of
// its return type.
//
// This rule was originally deferred because a naive implementation
// (`Resolver::resolveMember`, gated on `static` + `!returnOwned` + any
// `scoped` param) regressed `tests/sema/scoped_type_identity_2712_test.cpp`'s
// §27.2 function-signature-identity tests, which declared a static `Take`
// with a `scoped` parameter and no `owned` return. Those tests (and the
// analogous one in function_type_assignability_1544_test.cpp) are updated
// alongside this fix to give `Take` an `owned` return — legal under this
// rule — while continuing to exercise exactly the same §27.1/§27.2 identity
// behavior on the PARAMETER's `scoped` modifier. This file adds a
// self-contained regression test for that same interaction (§27.2 control
// below).
//
// The full front-end (through the ownership pass) runs on each program; the
// positives assert hasErrors, the negatives assert clean.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// §28.14 (v0.180): a `scoped` parameter on a static method with NO `owned`
// return is rejected — no call site could ever resolve a bounding object.
BEZEL_TEST(scoped_static_owned_return_1606,
          static_scoped_no_owned_return_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Take(scoped f: () -> ()) "
        "    f(); "
        "  end Take; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "static"));
    BEZEL_EXPECT_TRUE(diagContains(r, "`owned` return"));
}

// §28.14 (v0.180) control: a `scoped` parameter on a static method WITH an
// `owned` return is admitted — form 2/3 bounding is resolvable at qualifying
// call sites.
BEZEL_TEST(scoped_static_owned_return_1606,
          static_scoped_with_owned_return_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Take(scoped f: () -> ()): owned C "
        "    f(); "
        "    return new C(); "
        "  end Take; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 (v0.180) control: a NON-static (instance) method with a `scoped`
// parameter and no `owned` return is unaffected — form 5 resolves the
// receiver as the bounding object regardless of return type.
BEZEL_TEST(scoped_static_owned_return_1606,
          instance_scoped_no_owned_return_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Take(scoped f: () -> ()) "
        "    f(); "
        "  end Take; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 (v0.180) control: a static method with NO `scoped` parameter at all
// is unaffected by this rule regardless of its return type (the plain
// call-bounded default of §28.14).
BEZEL_TEST(scoped_static_owned_return_1606,
          static_plain_param_no_owned_return_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Take(f: () -> ()) "
        "    f(); "
        "  end Take; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.2 / §28.14 interaction (the reason this fix was deferred from #1541):
// a static method with a `scoped` parameter and an `owned` return still
// participates correctly in §27.1/§27.2 function-signature identity — the
// matching scoped-position method reference is accepted, and a plain-
// signature target (missing `scoped`) is rejected as an identity mismatch.
// This is a self-contained regression test for the exact interaction that
// caused the first #1541 attempt (commit d5066da) to be reverted.
BEZEL_TEST(scoped_static_owned_return_1606,
          spec_27_2_identity_still_works_with_owned_return) {
    auto matching = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Take(scoped f: () -> ()): owned C "
        "    f(); "
        "    return new C(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var g: (scoped () -> ()) -> () := C.Take; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(matching.diagnostics.hasErrors());

    auto mismatched = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Take(scoped f: () -> ()): owned C "
        "    f(); "
        "    return new C(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var h: (() -> ()) -> () := C.Take; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(mismatched.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(mismatched, "identity"));
}
