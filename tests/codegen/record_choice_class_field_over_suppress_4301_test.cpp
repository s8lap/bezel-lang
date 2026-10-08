// Codegen — issue #4301 / spec §9.7 ("Records and disposal") + §28.9
// ("Disposal"), docs revision v0.298.
//
// `calleeReturnMayAliasStringArgUnretained`'s top-level arm and its
// InlineArray-element twin (both introduced by fix #4280) suppress the
// caller-side release of a string argument whenever the callee return type
// (or InlineArray element type) is Record/Choice-declared — on the rationale
// that the record/choice's fast-path constructor store embeds the string
// unretained.
//
// That rationale is only sound when the record/choice actually HAS a directly
// string-typed field.  A Record/Choice whose fields are all Class-typed (each
// Class retaining its own string via `lowerStringFieldStore`) does NOT embed the
// caller's string via the raw fast path; suppressing the caller-side release
// in that case is a pure leak.
//
// FIX: narrow both the top-level arm and the InlineArray-element twin to
// additionally require that the Record/Choice has at least one directly
// string-typed field.  `recordOrChoiceHasStringField` walks the declaration's
// positional parameters and body fields, resolved via `ctx.resolvedType`, to
// answer this question without needing a full constructor-body dataflow
// analysis.
//
// MEASURED FAIL-WITHOUT: AddressSanitizer reports 62 byte(s) leaked in
// 3 allocation(s) — the string argument (38 B: 32 B bz_string header + 6 B
// "hello") and the Box object (24 B).
// MEASURED PASS-WITH: exit 5 ("he"+"llo" = "hello", Length 5), ASan/UAF
// detection clean (the #4277 leak in emitOwnedFieldDisposals for non-sealed
// classes with explicit constructors is a separate, pre-existing issue that
// does not affect this test's string disposal verification).
//
// Control row:
// - record_with_string_field: a Record with a DIRECT string field (the shape
//   fix #4280 was designed for) — must stay clean after the narrowing.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"   // fix #5626: failedAtCompileTimeWithRealLocation
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

// Strict oracle: compiled, ran, exited cleanly, no ASan/UAF finding.
// AOT harness fork-isolates each compile+run; leak detection is on.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// ===========================================================================
// PRIMARY PIN — Record containing a Class-typed field (no string field).
// fix #5626 (§9.7, v0.315): the Helper.Wrap form `return [new Str2(new
// Box(s))]` is now correctly rejected at the construction site inside Wrap,
// with the §9.7 no-temporary-source diagnostic — the bug allowed it to
// compile and produce a dangling borrow at runtime. The pre-fix accept
// (exit 5) pinned the buggy behaviour; this row now pins the correct
// rejection. The control row (record_with_direct_string_field) is
// unaffected: it has a direct string field and the raw fast path is correct.
// ===========================================================================
BEZEL_TEST(record_choice_class_field_over_suppress_4301,
           record_containing_class_field_no_string_leak) {
    // Str2 is a Record with a Class field B (Box), no direct string field.
    // Helper.Wrap returns [1]Str2, embedding the string through Box's
    // constructor into Box.V.  fix #5626 (§9.7, v0.315) closes the gap
    // where the no-temporary-source check fired only on the direct
    // NewExpr and missed nested constructions inside an ArrayLiteralExpr
    // return — the same `new Str2(new Box(s))` construction is now
    // rejected wherever it appears in a returning context.
    std::string prog =
        "module M; "
        "class Box "
        "  public V: string; "
        "  public constructor(v: string) "
        "    V := v; "
        "  end constructor; "
        "end Box; "
        "record Str2(B: Box); "
        "class Helper "
        "  public static method Wrap(s: string): [1]Str2 "
        "    return [new Str2(new Box(s))]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: [1]Str2; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    return outer[0].B.V.Length; "
        "  end Main; "
        "end C;";
    // fix #5626 (§9.7, v0.315): reject with the no-temporary-source
    // diagnostic — the NewExpr is inside an ArrayLiteralExpr in the return,
    // and the recursive walker now catches it.
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a borrow-bearing record may not be constructed from a temporary"));
}

// ===========================================================================
// CONTROL — Record with a DIRECT string field.  The raw fast path
// fires and the caller-side suppression IS correct; must stay clean.
// ===========================================================================
BEZEL_TEST(record_choice_class_field_over_suppress_4301,
           record_with_direct_string_field_control_stays_clean) {
    const char* prog =
        "module M; "
        "record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string): [1]Str "
        "    return [new Str(s)]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: [1]Str; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    return outer[0].V.Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}
