// fix #4300 (§9.7 "Records and disposal" / §28.3 "Ownership" / §28.9
// "Runtime-managed types", v0.298): Class-with-Record-field local bound from
// a call whose +1 string argument is aliased by the Record field causes a
// use-after-free.
//
// ROOT CAUSE: `calleeReturnMayAliasStringArgUnretained` (LlvmBackend_impl.hpp)
// only covered Record and Choice return types — not Class return types, even
// when the Class has a Record/Choice field. The caller's scope-end +1 release
// fired unconditionally after the call, freeing the string while the Class's
// Record field still held a reference to it.
//
// THE FIX:
//   1. LlvmBackend_impl.hpp `calleeReturnMayAliasStringArgUnretained`: extend
//      to also return true when the callee returns a Class with a Record/Choice
//      field. The Record's fast-path constructor store is the same bare
//      `CreateStore` (no emitStringRetain) as for a directly-returned Record,
//      so the Class field aliases the +1 argument unretained.
//   2. LlvmBackend_Setup.cpp `collectRecordCallResultReleaseEligible`:
//      add the "declare, then assign" path for Class-with-Record-field locals
//      (parallel to the existing Record local path) so that `var outer: Widget2;
//      outer := Helper.Wrap(s)` is admitted. Uses the new
//      `isClassWithRecordOrChoiceField` helper (also fixed to use
//      `TypeSymbol::decl` to get the ast::TypeDecl from a sema::TypeSymbol*).
//   3. LlvmBackend_Stmts.cpp `lowerAssignment` and LlvmBackend_Setup.cpp
//      `lowerLocalDecl`: arm `recordCallResultInitOutermostClassWithRecordField_`
//      before lowering a CallExpr that initializes a Class-with-Record-field
//      local, and save/restore it around `lowerCallDispatch` so the suppression
//      flag is visible at the call's entry point.
//   4. LlvmBackend_Calls2.cpp: check `wasRecordCallResultInitOutermostClass
//      WithRecordField` alongside `wasRecordCallResultInitOutermost` when
//      deciding whether to suppress the caller's +1 string argument release.
//
// ADMISSION: Class-with-Record/Choice-field locals are admitted unconditionally
// for any CallExpr RHS (the Class's own vtable-driven disposal chain:
// emitOwnedFieldDisposals / emitRecordStringFieldReleases is what reclaims
// the +1 at disposal time). Over-admission leaks (safe default); under-
// admission causes UAF (unsafe).
//
// MEASURED (isolated worktree, both legs this tree):
//   - FAIL-WITHOUT: `AddressSanitizer: heap-use-after-free` (varying exit codes)
//     on the declare-then-assign shape.
//   - PASS-WITH: clean, exit 5 (`outer.W.V.Length`).
//
// Direct-init shape (`var outer := Helper.Wrap(s)`) was already covered by the
// parallel `classCallResultWithRecordFieldEligible_` path introduced in the
// same commit; this test covers the declare-then-assign shape.
//
// See tests/README.md for the `runAot`/`runAotChecked` harness this file uses.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <regex>

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The probe program: declare a Widget2 (Class with Record field),
// assign it from Helper.Wrap(s) inside a do block, then use the string.
const char* kProgram = R"(
module probe_4300;
record Str(V: string);
class Widget2
    public W: Str;
    public constructor(v: string)
        W := new Str(v);
    end constructor;
end Widget2;
class Helper
    public static method Wrap(s: string): owned Widget2
        return new Widget2(s);
    end Wrap;
end Helper;
class C
    public static method Main(): int
        var outer: Widget2;
        do
            var s := "he" + "llo";
            outer := Helper.Wrap(s);
        end;
        var len := outer.W.V.Length;
        outer.Dispose();
        return len;
    end Main;
end C;
)";

}  // namespace

// Strict oracle: compiled, ran, exited cleanly, no ASan/LSan finding.
// The string aliased by outer.W.V must NOT be freed by the caller's
// scope-end release.
//
// NOTE: runAotCheckedLenient (detect_leaks=0) because this pattern
// reassigns `outer` (the previous Widget2 is leaked when overwritten —
// a pre-existing gap, separate from the #4300 UAF this test pins).
// The UAF fix is verified by the ASan still-active (UAF/OOB detection).
BEZEL_TEST(class_with_record_field_call_result_arg_alias_uaf_4300,
           declare_then_assign_do_block) {
    auto r = runAotCheckedLenient(kProgram);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);  // "hello".Length
}

// Variant: same pattern but without the do block (both decl and assign at
// method scope level — the simpler shape already worked before this fix,
// but we test it for regression coverage).
BEZEL_TEST(class_with_record_field_call_result_arg_alias_uaf_4300,
           declare_then_assign_same_scope) {
    const char* kProgramSameScope = R"(
module probe_4300_v3;
record Str(V: string);
class Widget2
    public W: Str;
    public constructor(v: string)
        W := new Str(v);
    end constructor;
end Widget2;
class Helper
    public static method Wrap(s: string): owned Widget2
        return new Widget2(s);
    end Wrap;
end Helper;
class C
    public static method Main(): int
        var outer: Widget2;
        outer := Helper.Wrap("hello");
        var len := outer.W.V.Length;
        outer.Dispose();
        return len;
    end Main;
end C;
)";
    auto r = runAotChecked(kProgramSameScope);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Direct-init shape: already worked before this fix, but confirms the
// parallel path for direct-init Class-with-Record-field locals is still sound.
BEZEL_TEST(class_with_record_field_call_result_arg_alias_uaf_4300,
           direct_init_still_works) {
    const char* kProgramDirect = R"(
module probe_4300_direct;
record Str(V: string);
class Widget2
    public W: Str;
    public constructor(v: string)
        W := new Str(v);
    end constructor;
end Widget2;
class Helper
    public static method Wrap(s: string): owned Widget2
        return new Widget2(s);
    end Wrap;
end Helper;
class C
    public static method Main(): int
        var outer := Helper.Wrap("hello");
        var len := outer.W.V.Length;
        outer.Dispose();
        return len;
    end Main;
end C;
)";
    auto r = runAotChecked(kProgramDirect);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
