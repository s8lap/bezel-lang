// Regression tests for #1107 / §28.5 (sweep #9, finding #7): a generic CLASS
// with an `owned T` field (T a type parameter) admits T = class/interface only.
// A `where T: class` constraint over-admits `string` and function types
// (§14.2), which are runtime-managed (§28.9) and carry no disposal obligation —
// so instantiating the generic with such a type bound to the owned field's
// parameter is a compile error at the instantiation site (§28.5 generic-owned-
// field check, paralleling the §14.4 owning-collection element rule). The
// compiler silently accepted `new Box<string>(…)`. Sema now rejects it at the
// type-resolution site (so it fires for every spelling — `new`, typed local,
// field, parameter). Found by adversarial discovery sweep #9.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
bool has285Reject(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("\xC2\xA7""28.5") != std::string::npos)
            return true;
    return false;
}
const char* kBoxOwned =
    "class Box<T> where T: class "
    "  private owned _item: T; "
    "  public constructor(owned item: T) _item := item; end constructor; "
    "end Box; ";
}  // namespace

// #1107/#7: `new Box<string>(…)` where Box has an `owned T` field → rejected.
BEZEL_TEST(generic_owned_field_string_reject_1107, string_new_rejected) {
    auto r = runAot(
        std::string("module M; ") + kBoxOwned +
        "class C public static method Main(): int "
        "  using var b := new Box<string>(\"hi\"); return 5; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has285Reject(r));
}

// #1107/#7: fires for a TYPED-LOCAL spelling too (not only `new`), like §14.4.
BEZEL_TEST(generic_owned_field_string_reject_1107, string_typed_local_rejected) {
    auto r = runAot(
        std::string("module M; ") + kBoxOwned +
        "class C public static method Main(): int "
        "  var b: Box<string>?; return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has285Reject(r));
}

// #1107/#7: a nullable owned field (`owned T?`) is likewise checked.
BEZEL_TEST(generic_owned_field_string_reject_1107, nullable_owned_string_rejected) {
    auto r = runAot(
        "module M; class Box<T> where T: class "
        "  private owned _item: T?; "
        "  public constructor() _item := null; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<string>(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has285Reject(r));
}

// #1107/#7: a function-typed argument is likewise rejected.
BEZEL_TEST(generic_owned_field_string_reject_1107, function_type_rejected) {
    auto r = runAot(
        std::string("module M; ") + kBoxOwned +
        "class C public static method Main(): int "
        "  var f: ()->int := lambda() => 1; "
        "  using var b := new Box<()->int>(f); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has285Reject(r));
}

// Regression guard: instantiating with a genuine CLASS type is accepted.
BEZEL_TEST(generic_owned_field_string_reject_1107, class_arg_accepted) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; class R public _v: int; "
                    "public constructor(v: int) _v := v; end constructor; "
                    "end R; ") + kBoxOwned +
        "class C public static method Main(): int "
        "  using var b := new Box<R>(new R(7)); return 7; "
        "end Main; end C;"), 7);
}

// Regression guard: a generic with a NON-owned `T` field accepts `string`
// (the check targets only `owned` fields, not borrowing/value fields).
BEZEL_TEST(generic_owned_field_string_reject_1107, non_owned_field_string_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> where T: class "
        "  private _item: T?; "
        "  public constructor() _item := null; end constructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<string>(); return 3; "
        "end Main; end C;"), 3);
}
