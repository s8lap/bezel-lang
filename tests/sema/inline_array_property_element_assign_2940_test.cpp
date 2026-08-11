// fix #2940 — assigning to an element of an inline-array-typed PROPERTY
// `obj.A[i] := v` was silently DROPPED with no diagnostic at any severity
// (spec §15.1 *Inline arrays*, §23.3 *Block-form properties*, §23.5
// *Restrictions*, §9.2, v0.240).
//
// WHAT THE SPEC REQUIRES.  §23.5 states the rule for the analogous case of a
// value-typed INDEXER result: "Assigning to a member of a value-typed indexer
// result is a compile error: `g[0].X := 5` would modify a discarded copy.
// Read, modify, write back through the setter."  The same rule applies to a
// value-typed PROPERTY result: a property GET returns a COPY of a value type
// (records/enums are value types, §9; inline arrays `[N]T` are value types
// too, §3.1), so `obj.A[1] := 7` writes an element of that discarded copy —
// the setter never fires.
//
// MEASURED on 8cc2b15 (the compiler this fix was written against), all
// SILENTLY DROPPED with no diagnostic:
//
//   using var p := new Plain();
//   p.A[1] := 7;                  // dropped — expected `z=0 a1=7`, got `z=0 a1=0`
//   i.A[1] := 9;                  // ditto, through an interface receiver
//
// THE CAUSE was the lowerStmt IndexExpr arm
// (`src/codegen/LlvmBackend_Stmts.cpp:374`, the `if (!addr2) return false;`)
// that called `lvalueAddr(*ix->target, arrSlotTy)`, recursing into the
// property. A property has no backing slot of its own, so the inner
// `lvalueAddr` returned null, and the caller's fallback silently dropped the
// store (`src/codegen/LlvmBackend_Layout.cpp:126-150`). No diagnostic was
// produced at any severity — a SILENTLY DROPPED write, the worst class of
// defect.
//
// The fix extends the §23.5 *member-of-value-typed-X* checks at
// `src/sema/ExprTypecheck.cpp:10399` (which already cover the
// member-of-indexer-result and member-of-property-result shapes for
// records/enums) to the inline-array-typed-PROPERTY element-write shape:
// `obj.A[i] := v` where `A: [N]T`. The diagnostic names the spec section
// (§23.5 / §15.1, #2940) and the correct spelling
// (bind a local, mutate it, write the whole array back through the setter).
//
// ⚠ MAINTENANCE NOTE — the REJECT assertions below pin that the property
// accessor MUST be invoked for inline-array element writes.  A later spec
// revision could ADMIT the sugar form "accept as read-modify-write" via a
// `lowerPropertyReadModifyWrite` triplet; if that lands, the reject tests
// below MUST be re-pointed at whatever shape remains disallowed (not deleted,
// not silently flipped to accept) so the closed-list assertion stays alive.
// The accept-controls (field inline-array element writes, interface-typed
// inline-array PROPERTY — same defect) are the guard that the closed list
// did not become an over-reject.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// True when the compile was rejected AND the rejection names §23.5/§15.1/
// #2940's inline-array-typed-PROPERTY element-write rule. Catches the
// non-overlap case where a different rule (a syntactic parse error, an
// unrelated type error) fires first.
bool rejectedBy2940(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("inline-array-typed property result") !=
                std::string::npos &&
            d.message.find("discarded copy") != std::string::npos &&
            d.message.find("§23.5") != std::string::npos &&
            d.message.find("§15.1") != std::string::npos &&
            d.message.find("#2940") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// =====================================================================
// #2940 — REJECT: the concrete inline-array-typed-PROPERTY element write
// `p.A[1] := 7`. Pre-fix this compiled clean (exit 0) with NO diagnostic
// at any severity, AND the program printed `z=0 a1=0` instead of
// `z=0 a1=7` — both the silent wrong value and the silent no-op at once.
// =====================================================================

BEZEL_TEST(sema_inline_array_property_element_assign_2940,
           concrete_inline_array_property_element_assign_rejected_spec_15_1) {
    JitResult r = runAot(
        "module IA2;"
        "public class Plain"
        "  public _z: int := 0;"
        "  public _a: [3]int;"
        "  public property A: [3]int"
        "    get => _a;"
        "    set => do _a := value; end;"
        "  end;"
        "end Plain;"
        "public class C"
        "  public static method Main(): int"
        "    using var p := new Plain();"
        "    p.A[1] := 7;"
        "    return 0;"
        "  end Main;"
        "end C;");
    BEZEL_EXPECT_TRUE(rejectedBy2940(r));
}

// =====================================================================
// #2940 — REJECT: the interface-typed-receiver spelling
// `i.A[1] := 9` where `i: IArr` and `IArr.A: [3]int`. Pre-fix the same
// silent-drop; #2931 made the inner lvalueAddr decline, so the interface
// spelling reproduces the concrete spelling identically.
// =====================================================================

BEZEL_TEST(sema_inline_array_property_element_assign_2940,
           interface_inline_array_property_element_assign_rejected_spec_15_1) {
    JitResult r = runAot(
        "module IA2;"
        "public interface IArr"
        "  property A: [3]int;"
        "end IArr;"
        "public class Impl: IArr"
        "  public _a: [3]int;"
        "  public property A: [3]int"
        "    get => _a;"
        "    set => do _a := value; end;"
        "  end;"
        "end Impl;"
        "public class C"
        "  public static method Main(): int"
        "    using var p: IArr := new Impl();"
        "    p.A[1] := 9;"
        "    return 0;"
        "  end Main;"
        "end C;");
    BEZEL_EXPECT_TRUE(rejectedBy2940(r));
}

// =====================================================================
// #2940 — REJECT: the nullable-element spelling
// `p.A?[1] := 9` where `A: [3]int?`. The nullable-element form keeps the
// value-type-of-the-inline-array result; the §23.5 discarded-copy rule
// applies identically.
// =====================================================================

BEZEL_TEST(sema_inline_array_property_element_assign_2940,
           nullable_element_property_element_assign_rejected_spec_15_1) {
    JitResult r = runAot(
        "module IA2;"
        "public class Plain"
        "  public _a: [3]int?;"
        "  public property A: [3]int?"
        "    get => _a;"
        "    set => do _a := value; end;"
        "  end;"
        "end Plain;"
        "public class C"
        "  public static method Main(): int"
        "    using var p := new Plain();"
        "    p.A[1] := 9;"
        "    return 0;"
        "  end Main;"
        "end C;");
    BEZEL_EXPECT_TRUE(rejectedBy2940(r));
}

// =====================================================================
// #2940 — ACCEPT control: an inline-array-typed FIELD (not a property)
// element write `p._a[1] := 7` STAYS accepted. A field IS an lvalue
// stored inline, so `p._a[1] := 7` writes through to the actual storage
// — the field-level emission in `lvalueAddr`'s inline-array branch
// reaches the slot directly. The diagnostic MUST NOT fire here.
// =====================================================================

BEZEL_TEST(sema_inline_array_property_element_assign_2940,
           inline_array_field_element_assign_accepted_spec_15_1) {
    JitResult r = runAot(
        "module IA2;"
        "public class Plain"
        "  public _a: [3]int;"
        "end Plain;"
        "public class C"
        "  public static method Main(): int"
        "    using var p := new Plain();"
        "    p._a[1] := 7;"
        "    return 0;"
        "  end Main;"
        "end C;");
    BEZEL_EXPECT_FALSE(rejectedBy2940(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// #2940 — ACCEPT control: a heap-collection-typed PROPERTY element write
// `p.A[1] := 7` where `A: Array<int>` STAYS accepted. A class-typed
// property GET yields a REFERENCE (§3.1), so the indexer setter is
// invoked via `lowerIndexerSet` — that path was already correct and the
// diagnostic MUST NOT fire here. (Pins the fix to inline-array-typed
// properties specifically, not all value-typed properties.)
// =====================================================================

BEZEL_TEST(sema_inline_array_property_element_assign_2940,
           heap_array_property_element_assign_accepted_spec_15_1) {
    JitResult r = runAot(
        "module IA2;"
        "public class C"
        "  public static method Main(): int"
        "    using var a := new Array<int>(3, 0);"
        "    a[1] := 7;"
        "    return 0;"
        "  end Main;"
        "end C;");
    BEZEL_EXPECT_FALSE(rejectedBy2940(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}