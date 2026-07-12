// fix #786 / §14.1: a DIRECT force-unwrap read of a generic type's nullable
// VALUE-type field (`b.V!`, not through a property) lowers with the field's
// concrete layout.
//
// `Emitter::fieldAddr` hand-rolled its generic substitution and installed only
// the LLVM-type map `genericSubst_`, never the sema-type map
// `genericSubstSemaTy_`. A nullable value field `V: T?` is laid out as the
// tagged struct `{i8, value}` whose `value` slot is chosen in lowerTypeRef's
// nullable bare-param branch FROM genericSubstSemaTy_; with that map empty it
// fell to the `{i8, i32}` placeholder. For `Box<bool>` the extracted value was
// `i32` while the force-unwrap branched on it (`br i32 %nv.val`), failing IR
// verification ("Branch condition is not 'i1' type!"). `int` worked only by
// luck (int == i32). This is the direct-field twin of #768 (property getter)
// and shares the mechanism of #766/#769. The fix builds genericSubstSemaTy_
// alongside genericSubst_ in all three fieldAddr branches and restores both.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §14.1: `Box<bool>().V!` — the bool? field's value slot is i1, so the
// force-unwrap branches on a valid i1 (was IR_FAIL: `br i32 %nv.val`).
BEZEL_TEST(generic_record_field_force_unwrap_786, bool_field) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T?); "
        "class P public static method Main(): int "
        "  var b := new Box<bool>(true); "
        "  if b.V! then return 7; end; "
        "  return 0; "
        "end Main; end P;"), 7);
}

// §14.1: a `long?` field — the value slot is i64; the unwrapped value flows
// through (was the i32 placeholder, truncating/garbling a 64-bit value). Use a
// 64-bit value that does NOT fit in i32 so an i32-placeholder slot would
// truncate it; compare with `= …L` (the long-test idiom) and return 0/1.
BEZEL_TEST(generic_record_field_force_unwrap_786, long_field) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T?); "
        "class P public static method Main(): int "
        "  var b := new Box<long>(5000000000L); "
        "  return (0 if b.V! = 5000000000L else 1); "
        "end Main; end P;"), 0);
}

// §14.1: the `int?` case still reads back (int == i32, the previously-lucky
// path) — guards against a regression that would drop the value.
BEZEL_TEST(generic_record_field_force_unwrap_786, int_field_unchanged) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T?); "
        "class P public static method Main(): int "
        "  var b := new Box<int>(42); "
        "  if b.V = null then return 1; end; "
        "  return b.V!; "  // 42
        "end Main; end P;"), 42);
}
