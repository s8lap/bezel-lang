// #6333 (§7.4, §4.4, §4.5, v0.338) — a TEST-ONLY commit: the issue's premise
// was measured and REFUTED, so what belongs in the tree is the measured rule,
// pinned symmetrically for both collection families so the asymmetry the issue
// reported cannot reappear.
//
// THE REPORT. #6333 claimed that §7.4 narrowing is not enforced on a nullable
// `OwningArray<T>` receiver — in both the instance form and the static-call
// sugar — citing `462d4f7c` (#6276) as the fix for the `Array<T>` twin.
//
// WHAT WAS MEASURED, and it contradicts the report on every point. All
// measurements are on this tree, and the first two were repeated at the base
// the issue names (389ca74b) by checking out that commit's
// src/sema/ExprTypecheck_Calls.cpp and rebuilding.
//
//   1. NO `Array`/`OwningArray` ASYMMETRY. Every row below is run for BOTH
//      families and the two agree in every cell. #6276's guard
//      (ExprTypecheck_Calls.cpp, `(colName == "Array" ||
//      colName == "OwningArray")` keyed on `argTy->headName() == colName`)
//      covers both identically by construction.
//
//   2. THE INSTANCE FORM WAS ALREADY REJECTED AT THE CITED BASE.
//      `Go(n: OwningArray<Item>?) { n.Reverse(); }` rejects at 389ca74b, not
//      only at HEAD. `git merge-base --is-ancestor 462d4f7c 389ca74b` is
//      false: #6276 landed AFTER the base the issue cites, so the base had no
//      guard for either family, and the instance form was rejected anyway.
//
//   3. THE ISSUE'S OWN REPRO IS CORRECT. Its local was
//      `var n: OwningArray<Item>? := new OwningArray<Item>();` — the
//      initialiser is what establishes §7.4 narrowing, so the member call is
//      admitted, and that is the rule, not an escape from it. The `Array<Item>`
//      twin of that program compiles identically.
//
// So there is no `src/` change for #6333. These rows exist so that the
// measured rule is a pinned contract rather than a recollection: the narrowing
// is admitted for a local that its initialiser narrowed, refused for a
// parameter, admitted again for the null-conditional form the diagnostic
// recommends, and — critically — IDENTICAL for `Array<T>` and `OwningArray<T>`.
//
// WHAT IS DELIBERATELY NOT PINNED. A local re-assigned to `null` after its
// narrowing was established was filed as #6346 as this file's "residual that IS
// real". #6346 has since been MEASURED and is REFUTED: its repro wrote
// `n = null;`, and `=` is §8.4's EQUALITY operator, not an assignment — it
// stores nothing, so the narrowing it appeared to leave stale was never stale.
// The assignment spelling `n := null;` (and `n ??= null;`) DOES invalidate the
// narrowing, and the member call after it is refused for both families. The
// measured rule, and the runtime proof that `=` does not assign, are pinned in
// tests/sema/reassignment_of_null_refuted_6346_test.cpp — so the row this
// paragraph originally deferred belongs THERE, not here.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// The §7.4 diagnostic a nullable receiver produces, in the part BOTH
// spellings share. MEASURED: the instance form reads "member access
// `.Reverse` on a nullable value requires narrowing it to non-null first
// (§7.4): ...", while the static-call sugar reads "the `Array.Reverse(`
// argument is the receiver of the equivalent instance call ..., and it is read
// at type `Array?` — a nullable value requires narrowing it to non-null first
// (§7.4): ...". The two differ in their opening clause, so the needle is the
// clause they share; a rename there is a behaviour flip for every reject row
// below.
const char* const kNarrowNeedle = "requires narrowing it to non-null first";

// Both families, so no row can be asymmetric by accident. `Array<T>` is
// stdlib §4.4 and `OwningArray<T>` is §4.5; `Item` is a plain class, so
// neither type's surface is conditionally admitted at this element type.
const char* const kFamilies[] = {"Array", "OwningArray"};

std::string withFamily(const char* family, const std::string& rest) {
    std::string out = "public class Item\nend Item;\n";
    out += rest;
    // Substitute the family into the single `FAMILY` slot the body carries.
    const std::string slot = "FAMILY";
    std::string::size_type at = out.find(slot);
    while (at != std::string::npos) {
        out.replace(at, slot.size(), family);
        at = out.find(slot, at + std::string(family).size());
    }
    return out;
}

// A member call on a nullable RECEIVER is rejected, and rejected with the
// §7.4 diagnostic — not by crashing and not with an unrelated error.
void expectReceiverRejected(const std::string& classes, const std::string& body) {
    auto r = runAotChecked("module M;\n" + classes + body);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNarrowNeedle));
}

}  // namespace

// s7_4_0001: the rule the issue got right. A local whose INITIALISER narrows it
// (§7.4) admits the member call. Run for both families: this is the row that
// makes the "no asymmetry" claim a measurement rather than an assertion.
BEZEL_TEST(collection_receiver_narrowing_refuted_6333,
           initialiser_narrowed_local_admits_the_member_for_both_families_spec_7_4) {
    for (const char* family : kFamilies) {
        auto r = runAotChecked(
            "module M;\n" + withFamily(family,
                "public class Program\n"
                "  public static method Main(): int\n"
                "    var n: FAMILY<Item>? := new FAMILY<Item>();\n"
                "    n.Reverse();\n"
                "    return 0;\n"
                "  end Main;\n"
                "end Program;\n"));
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
    }
}

// s7_4_0002: the rule the issue got wrong, in the INSTANCE form. A PARAMETER's
// declared type is `FAMILY<Item>?` and nothing narrows it, so §7.4 refuses the
// member call. MEASURED as ALREADY REJECTED at 389ca74b, the base the issue
// cites, for both families and for both spellings — so this row is not
// crediting #6276 with a fix it did not make.
BEZEL_TEST(collection_receiver_narrowing_refuted_6333,
           parameter_receiver_refused_in_both_families_spec_7_4) {
    for (const char* family : kFamilies) {
        expectReceiverRejected(
            withFamily(family,
                "class Go\n"
                "  public method G(n: FAMILY<Item>?): int\n"
                "    n.Reverse();\n"
                "    return 0;\n"
                "  end G;\n"
                "end Go;\n"
                "public class Program\n"
                "  public static method Main(): int\n"
                "    using var g := new Go();\n"
                "    return g.G(null);\n"
                "  end Main;\n"
                "end Program;\n"),
            "");
    }
}

// s7_4_0003: the STATIC-CALL SUGAR spelling of s7_4_0002. The issue named the
// sugar separately; it is the same §7.4 verdict for the same reason, and
// asserting it separately is what shows the two spellings were both measured
// rather than one standing in for the other.
BEZEL_TEST(collection_receiver_narrowing_refuted_6333,
           parameter_receiver_refused_in_the_static_call_sugar_too_spec_7_4) {
    for (const char* family : kFamilies) {
        expectReceiverRejected(
            withFamily(family,
                "class Go\n"
                "  public method G(n: FAMILY<Item>?): int\n"
                "    FAMILY.Reverse(n);\n"
                "    return 0;\n"
                "  end G;\n"
                "end Go;\n"
                "public class Program\n"
                "  public static method Main(): int\n"
                "    using var g := new Go();\n"
                "    return g.G(null);\n"
                "  end Main;\n"
                "end Program;\n"),
            "");
    }
}

// s7_4_0004: the REMEDY the §7.4 diagnostic itself names. A null-conditional
// call is the documented way to reach a member on a possibly-null receiver, so
// it must be admitted where the bare member call is refused. Without this row
// a fix could "simplify" the rule into refusing both.
BEZEL_TEST(collection_receiver_narrowing_refuted_6333,
           null_conditional_receiver_is_admitted_in_both_families_spec_7_4) {
    for (const char* family : kFamilies) {
        // The argument is `null`, NOT an inline `new FAMILY<Item>()`: an
        // owned array built at the call site and handed to a parameter is
        // disposed by the PARAMETER's scope, and `runAotChecked` (which runs
        // LSan) MEASURED that as "sanitizer fired (LSan/ASan at exit)" — the
        // leak is this row's construction, not a compiler defect, so the row
        // passes null and the receiver being null is exactly what the
        // null-conditional form is for.
        auto r = runAotChecked(
            "module M;\n" + withFamily(family,
                "class Go\n"
                "  public method G(n: FAMILY<Item>?): int\n"
                "    n?.Reverse();\n"
                "    return 0;\n"
                "  end G;\n"
                "end Go;\n"
                "public class Program\n"
                "  public static method Main(): int\n"
                "    using var g := new Go();\n"
                "    return g.G(null);\n"
                "  end Main;\n"
                "end Program;\n"));
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
    }
}