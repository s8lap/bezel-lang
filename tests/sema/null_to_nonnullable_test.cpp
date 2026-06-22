// Sema — discovery sweep #7: assigning the `null` literal (typed `Object?`) to a
// NON-nullable target was wrongly accepted, then miscompiled:
//   `var x: int := null;`  → malformed IR (the Object early-return in
//                            checkAssignable preempted the nullable check).
//   `var x: Box := null;`  → accepted, then `x.value` SIGSEGV'd at runtime (the
//                            Object?-source → Named-target case had no branch).
// checkAssignable now rejects a nullable source into a non-nullable target while
// still accepting null into a nullable target (`int?`/`Box?`/`string?`), the
// Object unbox, and numeric widening. §5.1 / §7.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}
std::string mod(const std::string& body) {
    return std::string(
        "module M; class Box public value: int := 0; "
        "  public constructor(v: int) value := v; end constructor; end Box; "
        "class C public static method Main(): int ") + body + " end Main; end C;";
}
}  // namespace

// REJECT: null into a non-nullable value type.
BEZEL_TEST(null_to_nonnullable, int_diagnoses) {
    auto r = parseSemaAll(mod("var x: int := null; return x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// REJECT: null into a non-nullable reference type.
BEZEL_TEST(null_to_nonnullable, class_diagnoses) {
    auto r = parseSemaAll(mod("var x: Box := null; return x.value;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// REJECT: null into a non-nullable string.
BEZEL_TEST(null_to_nonnullable, string_diagnoses) {
    auto r = parseSemaAll(mod("var x: string := null; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ACCEPT: null into nullable targets (value, reference, string).
BEZEL_TEST(null_to_nonnullable, nullable_targets_ok) {
    auto r = parseSemaAll(mod(
        "var a: int? := null; var b: Box? := null; var c: string? := null; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ACCEPT: the Object unbox and numeric widening are unaffected.
BEZEL_TEST(null_to_nonnullable, unbox_and_widen_ok) {
    auto r = parseSemaAll(mod(
        "var o: Object := 5; var oq: Object? := null; var lng: long := 7; "
        "return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
