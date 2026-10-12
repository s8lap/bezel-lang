// Codegen — the #5611 SPELLING class, audit follow-up (per §27.6 / §28.14 /
// §28.9, v0.334): codegen asks "is this formal function-typed?" from the AST
// SPELLING (`p.type->kind == ast::TypeKind::Function`), while sema asks the
// same question from the RESOLVED type (`ctx.resolvedType`), which is
// alias-transparent. An A.2 `TypeAliasDecl` is spelled `Named` in the AST but
// resolves to its target, so
//
//     type Show := (int) -> string;
//     method Take(scoped f: Show) end Take;
//     p.Take(lambda (x: int): string => $"n={x}");
//
// disagreed with sema on a function-typed formal. That disagreement governs
// the §28.14 call-bounded RELEASE, not a lowering detail.
//
// THE COORDINATOR'S FIX (#5611 proper) introduced `resolvedFormalIsFunction`
// and applied it at three named sites: lowerCallDispatch's sync-iterator arm,
// its call-bounded arm, and `isCallBoundedClosureArgPosition`. THIS audit
// found two further gates on the same decision that the three-site fix left on
// the AST spelling — the `scoped` arm in LlvmBackend_Calls2.cpp and the
// ctor-formal arm in LlvmBackend_Exprs2.cpp (whose own comment calls itself
// the transplanted twin of lowerCall's release, applying the AST-shape guard
// "one-for-one"). Both are fixed here by REUSING the sanctioned accessor.
//
// MEASURED (LSan, 500 iterations x 48 B = 24000 B expected on the alias
// rows): each row below is a PAIR — the identical program with the function
// type spelled inline, and with it spelled through an alias. The inline row
// is the control that proves the harness runs; the alias row is the defect. A
// pair whose control is RED is a broken row, not a finding.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan WITH
// LeakSanitizer on: a leak, a double free or a use-after-free fails the row
// before the printed exit value is compared. ASan exits 1 on a memory error,
// so the expected "exit=0" cannot be reached by a leaking row.
std::string checkedExit(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return std::string("<did-not-run>");
    return "exit=" + std::to_string(r.exitCode);
}
}  // namespace

// ── Row 1/2 pair — `scoped` formal (LlvmBackend_Calls2.cpp). A §28.14
// storage-bounded parameter admits storage, so the caller still owns the cell
// when the callee returns and it must be released at the call's end. This arm
// bails at ANY body reference (conservative: leak, not UAF), and this
// empty-body callee references nothing — so before the fix the alias spelling
// skipped the arm entirely: 24000 B / 500 allocations, against a clean run for
// the identical program with the type spelled inline.
BEZEL_TEST(alias_formal_release_scoped_and_ctor_5611_spelling,
           control_inline_function_type_scoped_formal_released_28_14) {
    std::string src =
        "module M; "
        "class Program "
        "  method Take(scoped f: (int) -> string) end Take; "
        "  public static method Main(): int "
        "    var p := new Program(); "
        "    for var j in 0..<500 do "
        "      p.Take(lambda (x: int): string => $\"n={x}\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=0");
}

BEZEL_TEST(alias_formal_release_scoped_and_ctor_5611_spelling,
           alias_to_function_type_scoped_formal_released_28_14) {
    std::string src =
        "module M; "
        "type Show := (int) -> string; "
        "class Program "
        "  method Take(scoped f: Show) end Take; "
        "  public static method Main(): int "
        "    for var j in 0..<500 do "
        "      var p := new Program(); "
        "      p.Take(lambda (x: int): string => $\"n={x}\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=0");
}

// ── Row 3/4 pair — ctor formal (LlvmBackend_Exprs2.cpp). The ctor arm is the
// transplanted twin of lowerCall's call-bounded release. An empty ctor body
// never invokes the parameter either, so the caller keeps the only reference.
BEZEL_TEST(alias_formal_release_scoped_and_ctor_5611_spelling,
           control_inline_function_type_ctor_formal_released_28_14) {
    std::string src =
        "module M; "
        "class Holder "
        "  public constructor(f: (int) -> string) "
        "  end constructor; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    for var j in 0..<500 do "
        "      var h := new Holder(lambda (x: int): string => $\"n={x}\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=0");
}

BEZEL_TEST(alias_formal_release_scoped_and_ctor_5611_spelling,
           alias_to_function_type_ctor_formal_released_28_14) {
    std::string src =
        "module M; "
        "type Show := (int) -> string; "
        "class Holder "
        "  public constructor(f: Show) "
        "  end constructor; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    for var j in 0..<500 do "
        "      var h := new Holder(lambda (x: int): string => $\"n={x}\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=0");
}

// ── Row 5/6 pair — NEGATIVE CONTROL for the two already-fixed sites
// (lowerCallDispatch's two `formalIsFunction` arms). The coordinator's
// three-site fix covers them; these rows pin that they stay closed, so a
// future refactor that narrows the accessor back to the AST spelling shows up
// as a RED control here rather than a silent pass. Both spellings must be
// clean — if the alias row ever leaks, the sanctioned fix regressed.
BEZEL_TEST(alias_formal_release_scoped_and_ctor_5611_spelling,
           control_inline_function_type_method_formal_released_28_14) {
    std::string src =
        "module M; "
        "class Program "
        "  method Take(f: (int) -> string) end Take; "
        "  public static method Main(): int "
        "    var p := new Program(); "
        "    for var j in 0..<500 do "
        "      p.Take(lambda (x: int): string => $\"n={x}\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=0");
}

BEZEL_TEST(alias_formal_release_scoped_and_ctor_5611_spelling,
           already_fixed_method_formal_alias_spelling_stays_released_28_14) {
    std::string src =
        "module M; "
        "type Show := (int) -> string; "
        "class Program "
        "  method Take(f: Show) end Take; "
        "  public static method Main(): int "
        "    var p := new Program(); "
        "    for var j in 0..<500 do "
        "      p.Take(lambda (x: int): string => $\"n={x}\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(checkedExit(src), "exit=0");
}