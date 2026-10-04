// Spec-derived codegen tests for #5269/#5273 — §19.3 / §7.11 / §10
// (v0.314 ruling): a relational pattern over an enum scrutinee is a compile
// error naming the type — an enum has no intrinsic ordering (§19.3). The
// verdict is keyed on the TYPE, never on whether a declaration supplies
// ordering operators. These rows pin the reject verdict.
//
// runAot drives the real AOT path (fork-isolated Driver::compileFile + clang
// link + native exec) — the strict oracle. Each row asserts compile-time
// rejection.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// §19.3 (v0.314, #5269): checks for the ordering-diagnostic message.
bool hasOrderingDiag(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("has no intrinsic ordering (\xC2\xA7""19.3)") !=
                std::string::npos)
            return true;
    return false;
}

}  // namespace

// §19.3 (v0.314, #5269): an enum has no intrinsic ordering. A relational
// pattern over an enum scrutinee is a compile error naming "E".
BEZEL_TEST(scrutinee_enum_narrowed_relop_5173,
           narrowed_enum_scrutinee_gt_rejected_5173) {
    auto r = runAot(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.B; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of > E.A => 1, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasOrderingDiag(r));
}

// §19.3 (v0.314, #5269): same — the relational pattern is rejected regardless
// of which arm it would select.
BEZEL_TEST(scrutinee_enum_narrowed_relop_5173,
           narrowed_enum_scrutinee_gt_no_match_rejected_5173) {
    auto r = runAot(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.A; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of > E.A => 1, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasOrderingDiag(r));
}

// §19.3 (v0.314, #5269): `<=` form.
BEZEL_TEST(scrutinee_enum_narrowed_relop_5173,
           narrowed_enum_scrutinee_le_boundary_rejected_5173) {
    auto r = runAot(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.B; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of <= E.B => 8, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasOrderingDiag(r));
}

// §19.3 (v0.314, #5269): `<` and `>=` spellings — all forms are rejected.
BEZEL_TEST(scrutinee_enum_narrowed_relop_5173,
           narrowed_enum_scrutinee_lt_and_ge_rejected_5173) {
    auto r1 = runAot(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.A; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of < E.B => 4, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r1.ran);
    BEZEL_EXPECT_TRUE(hasOrderingDiag(r1));

    auto r2 = runAot(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.C; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of >= E.C => 6, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r2.ran);
    BEZEL_EXPECT_TRUE(hasOrderingDiag(r2));
}