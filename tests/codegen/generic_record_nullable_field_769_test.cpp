// fix #769 / §9.1 / §14.1: a generic record's nullable VALUE-type field `V: T?`
// stores and reads back the correct value for a concrete instantiation.
//
// For `record Box<T>(V: T?)` instantiated as `Box<int>`, the field `V` is
// `int?` — a tagged `{i8 hasValue; int value}` struct. The bug: buildRecordValue
// used `recordMemberType` (the UNRESOLVED `T?`), so the nullable-value wrap was
// mis-judged and the value was stored wrong (b.V! came back wrong). And
// recordMemberTypeResolved dropped the field's `?` when substituting T (yielding
// the bare `int`, not `int?`). The fix uses recordMemberTypeResolved AND
// preserves the nullable flag through the substitution, so `V: T?` resolves to
// `int?`.

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

// §9.1: `new Box<int>(99)` — V is non-null and unwraps to 99. (The #769 repro.)
BEZEL_TEST(generic_record_nullable_field_769, present_value_reads_back) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T?); "
        "class P public static method Main(): int "
        "  var b := new Box<int>(99); "
        "  if b.V = null then return 10; end; "
        "  var v := b.V!; "
        "  if v = 99 then return 0; end; "
        "  return 1; "
        "end Main; end P;"), 0);
}

// §9.1: `new Box<int>(null)` — the nullable value field is genuinely null.
BEZEL_TEST(generic_record_nullable_field_769, null_value_is_null) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Box<T>(V: T?); "
        "class P public static method Main(): int "
        "  var b := new Box<int>(null); "
        "  return 0 if b.V = null else 1; "
        "end Main; end P;"), 0);
}

// §9.1: a generic record mixing a non-generic `int` field with a generic `T?`
// value field — both read back correctly (the nullable-value wrap fires only on
// the T? slot, and the layout of both slots is right). 7 + 42 = 49.
BEZEL_TEST(generic_record_nullable_field_769, mixed_fields_read_back) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Rec<T>(id: int, val: T?); "
        "class P public static method Main(): int "
        "  var r := new Rec<int>(7, 42); "
        "  if r.val = null then return 9; end; "
        "  return r.id + r.val!; "  // 49
        "end Main; end P;"), 49);
}
