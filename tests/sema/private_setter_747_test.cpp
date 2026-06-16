// Spec-derived sema tests for fix #747 (R42 FALSE_ACCEPT) — a property write
// must respect the setter accessor's visibility.
//
// Authority: §13.6 / §13.1 — an accessor may be declared more restrictive than
// the property (e.g. `private set`); the access rules then apply to the
// accessor. Bug: a `private set` was writable from any class — the write
// compiled and took effect. The fix computes the setter's effective visibility
// and rejects an inaccessible write; a within-class write and a public setter
// are unaffected.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasSetterVisError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("its setter is not accessible") != std::string::npos)
            return true;
    return false;
}

const char* kCounterPrivateSet =
    "class Counter "
    "  public _v: int := 0; "
    "  public property Value: int get => _v; "
    "    private set => do _v := value; end; end; "
    "end Counter; ";

}  // namespace

// §13.6: writing a `private set` property from another class is rejected.
BEZEL_TEST(private_setter_747, write_from_other_class_rejected) {
    auto r = parseSemaAll(
        std::string("module M; ") + kCounterPrivateSet +
        "class Program public static method Main(): int "
        "  using var c := new Counter(); c.Value := 99; return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(hasSetterVisError(r));
}

// §13.6: writing the `private set` from WITHIN the declaring class is allowed.
BEZEL_TEST(private_setter_747, write_within_class_ok) {
    auto r = parseSemaAll(
        std::string("module M; ") +
        "class Counter "
        "  public _v: int := 0; "
        "  public property Value: int get => _v; "
        "    private set => do _v := value; end; end; "
        "  public method Bump() this.Value := _v + 1; end Bump; "
        "end Counter; "
        "class Program public static method Main(): int return 0; end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(hasSetterVisError(r));
}

// §13.6: a public setter is writable from anywhere — no error.
BEZEL_TEST(private_setter_747, public_setter_ok) {
    auto r = parseSemaAll(
        "module M; class Counter "
        "  public _v: int := 0; "
        "  public property Value: int get => _v; set => do _v := value; end; end; "
        "end Counter; "
        "class Program public static method Main(): int "
        "  using var c := new Counter(); c.Value := 42; return c.Value; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(hasSetterVisError(r));
}
