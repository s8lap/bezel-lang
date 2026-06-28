// Spec-derived sema tests for fix #747 (R42 FALSE_ACCEPT) — assignment to a
// property that has no setter.
//
// Authority: §23.3 — "A read-only property in block form omits set." An
// expression-bodied property (`P: T => e;`) is likewise read-only. Bug:
// assigning to such a setterless property was silently accepted and dropped at
// runtime. The fix rejects it; read-write block properties and plain
// auto-properties (which have an implicit setter) are unaffected.

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

bool hasNoSetterError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("it has no setter") != std::string::npos)
            return true;
    return false;
}

// §23 (#1192): the mirror diagnostic — reading a write-only (set-only) property.
bool hasWriteOnlyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is write-only") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §23.3: assignment to a block-form getter-only property is rejected.
BEZEL_TEST(getter_only_property_747, block_getter_only_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property Val: int get => _x; end; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Val := 99; return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNoSetterError(r));
}

// §23.3: assignment to an expression-bodied property is rejected.
BEZEL_TEST(getter_only_property_747, expression_bodied_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 5; "
        "  public property Val: int => _x; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Val := 9; return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNoSetterError(r));
}

// §23.3: a block property WITH a setter is assignable — no error.
BEZEL_TEST(getter_only_property_747, block_read_write_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property Val: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Val := 42; return c.Val; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNoSetterError(r));
}

// §23.3: a plain auto-property has an implicit setter — no error.
BEZEL_TEST(getter_only_property_747, auto_property_ok) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public property Val: int; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Val := 7; return c.Val; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNoSetterError(r));
}

// §23 (#1192): reading a write-only (set-only block) property is rejected — it
// has no getter. (Previously accepted, silently yielding 0.)
BEZEL_TEST(getter_only_property_747, set_only_read_rejected_1192) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property Val: int set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Val := 7; return c.Val; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasWriteOnlyError(r));
}

// §23 (#1192): a compound `op=` on a set-only property READS first, so it is
// also rejected (the plain `:=` exemption does not extend to compound forms).
BEZEL_TEST(getter_only_property_747, set_only_compound_assign_rejected_1192) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property Val: int set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Val += 3; return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasWriteOnlyError(r));
}

// §23 (#1192): a plain `:=` write to a set-only property is NOT a read — it is
// dispatched to the setter and must be accepted (no write-only error).
BEZEL_TEST(getter_only_property_747, set_only_plain_write_ok_1192) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property Val: int set => do _x := value; end; end; "
        "  public method Show(): int => _x; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Val := 7; return c.Show(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasWriteOnlyError(r));
}

// §23 (#1192): a read-write (get+set) property read is unaffected — no error.
BEZEL_TEST(getter_only_property_747, get_set_read_ok_1192) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property Val: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    using var c := new C(); c.Val := 9; return c.Val; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasWriteOnlyError(r));
}
