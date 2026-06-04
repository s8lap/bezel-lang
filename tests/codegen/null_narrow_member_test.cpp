// Codegen JIT — nullability flow-narrowing (§7.4 / §7.8 / §8.4; issue #418).
//
// §7.4: member access on a nullable value is legal once the compiler has
// narrowed it to non-null. The narrowing forms exercised here:
//   * `if x <> null then x.field` — then-block narrows (§7.4 canonical);
//   * `if x = null then ... else x.field` — else-block narrows (§8.4);
//   * `x!.field` — `!` force-unwrap narrows (§7.8);
//   * `a <> null and b <> null` — `and`-joined null tests both narrow.
// These programs must COMPILE (sema accepts the narrowed access) and produce
// the correct value at runtime. Observable via the int exit code; `ran` stays
// false if sema wrongly rejects the narrowed access.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// A nullable class parameter narrowed via `<> null`.
const char* kUser =
    "module M; class User public Name: int := 0; end User; ";

}  // namespace

// §7.4: `if user <> null then user.Name` reads the field of the non-null
// payload. The user is non-null here, so the then-block runs and returns 42.
BEZEL_TEST(codegen_null_narrow, ne_null_then_reads_field_spec_7_4) {
    BEZEL_EXPECT_EQ(run(std::string(kUser) +
        "class C public static method Main(): int "
        "  var u := new User(); u.Name := 42; var p: User? := u; "
        "  if p <> null then return p.Name; end; return -1; "
        "end Main; end C;"), 42);
}

// §7.4: the same guard takes the else/fall-through path when the value is null.
BEZEL_TEST(codegen_null_narrow, ne_null_null_falls_through_spec_7_4) {
    BEZEL_EXPECT_EQ(run(std::string(kUser) +
        "class C public static method Main(): int "
        "  var p: User? := null; "
        "  if p <> null then return p.Name; end; return 7; "
        "end Main; end C;"), 7);
}

// §8.4: `if p = null then ... else p.Name` — the else-block narrows, so the
// member read returns the payload field (42).
BEZEL_TEST(codegen_null_narrow, eq_null_else_reads_field_spec_8_4) {
    BEZEL_EXPECT_EQ(run(std::string(kUser) +
        "class C public static method Main(): int "
        "  var u := new User(); u.Name := 42; var p: User? := u; "
        "  if p = null then return -1; else return p.Name; end; "
        "end Main; end C;"), 42);
}

// §7.8: `p!.Name` force-unwraps and reads the field. The payload is non-null.
BEZEL_TEST(codegen_null_narrow, bang_unwrap_reads_field_spec_7_8) {
    BEZEL_EXPECT_EQ(run(std::string(kUser) +
        "class C public static method Main(): int "
        "  var u := new User(); u.Name := 9; var p: User? := u; "
        "  return p!.Name; "
        "end Main; end C;"), 9);
}

// §7.4 / §12.7: `a <> null and b <> null` narrows both inside the combined
// then-block; the two field reads sum.
BEZEL_TEST(codegen_null_narrow, and_joined_both_narrow_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class A public X: int := 0; end A; "
        "class B public Y: int := 0; end B; "
        "class C public static method Main(): int "
        "  var ao := new A(); ao.X := 11; var a: A? := ao; "
        "  var bo := new B(); bo.Y := 22; var b: B? := bo; "
        "  if a <> null and b <> null then return a.X + b.Y; end; "
        "  return -1; "
        "end Main; end C;"), 33);
}

// §7.4: a nullable RECORD value narrowed via `<> null` reads its field.
BEZEL_TEST(codegen_null_narrow, nullable_record_narrowed_read_spec_7_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(V: int); "
        "class C public static method Main(): int "
        "  var b: Box? := new Box(7); "
        "  if b <> null then return b.V; end; return -1; "
        "end Main; end C;"), 7);
}
