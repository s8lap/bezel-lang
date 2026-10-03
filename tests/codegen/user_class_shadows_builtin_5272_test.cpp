// Regression tests for #5272 — a user class that takes a BUILTIN's spelling did
// not shadow the builtin; codegen's static-call dispatch is keyed on the
// receiver's identifier TEXT, so it lowered the builtin member and silently
// discarded the user's own `MethodSymbol` that sema had already resolved.
//
// WHAT THE DOCUMENTS REQUIRE.  Spec §2.3 (v0.313, *Always-available names*):
// of the always-available names, only §3.4's reserved row — the PascalCase
// aliases of the primitive value types, plus `String` — cannot be shadowed;
// "every other always-available name may be shadowed by local declarations
// under the normal name-resolution rules", which §2.7 (*Name resolution for
// dotted names*) supplies: the current-module declaration wins the bare name
// and the auto-imported one stays reachable by its qualified name
// (`System.Convert.ToInt32`). `Convert` and `Math` are in neither reserved set,
// so a user class may take those spellings and MUST win. Spec §22.4 (v0.313,
// *Inline `out` declarations*) types the `out var d` slot from the method's own
// `out` parameter declaration — the USER's `out r: int`, which is what the
// builtin arm overran.
//
// The defect is codegen-side only: sema resolved `Convert.TryToDouble` to the
// user's `MethodSymbol` correctly; `LlvmBackend_Calls.cpp` derived `cls` from
// `id->name` and the ~24 `cls == "<Builtin>"` arms matched on that text.
//
// MEASURED PRE-FIX (staging r23-c28 ccd0be14, `agent_check --probe --asan`,
// and this suite's `runAotChecked`):
//   * user `class Convert` + static `TryToDouble(s: string, out r: int): bool`
//     → `AddressSanitizer: stack-buffer-overflow ... WRITE of size 8` in
//     `main`: the builtin `strtod` arm ran and wrote a `double` (8 bytes)
//     through the 4-byte `int` out-slot the USER signature declared;
//   * user `class Math` + static `Sqrt(d: double): double` returning 7.0 →
//     `Math.Sqrt(2.0)` printed `1.4142135623730951` (the builtin arm) and the
//     user's method never ran — the same shadowing with NO memory fault, so it
//     is a silent-wrong-answer row and pins the family, not just Convert.
//
// The fix gates the text-keyed arms on sema's resolution: a receiver may reach
// them only when its `TypeSymbol` is the synthetic `builtinClassSym`, which is
// shaped `decl == nullptr && moduleName.empty()` (the #2997 discriminator). A
// user's TypeSymbol carries a real `ast::TypeDecl` in-module, or a populated
// moduleName when imported.
//
// The oracle is stdout byte-for-byte AND exit 0. The Convert rows run under
// `runAotChecked` (generated-code ASan) because the pre-fix defect is an
// out-of-bounds WRITE that plain `runAot` reports only as a wrong value.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A module carrying one user class declared OUTSIDE Main (so the class name is
// in scope for a static call) plus the usual Main. `decl` is the user's class.
std::string prog(const std::string& decl, const std::string& body) {
    return std::string("module M; ") + decl +
           " public class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

void expectOut(const std::string& decl, const std::string& body,
               const std::string& want) {
    auto r = runAot(prog(decl, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// The same row under generated-code ASan — the pre-fix Convert defect is an
// out-of-bounds WRITE, so only the instrumented leg makes it observable as the
// memory error it is rather than as a merely wrong value.
void expectOutAsan(const std::string& decl, const std::string& body,
                   const std::string& want) {
    auto r = runAotChecked(prog(decl, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// The issue's own repro, verbatim: a user `Convert` with an `out r: int`
// whose slot the builtin `strtod` arm overran by 4 bytes.
const char* kUserConvert =
    "public class Convert public static method TryToDouble(s: string, "
    "out r: int): bool r := 41; return true; end TryToDouble; end Convert; ";

}  // namespace

// ---- #5272: the user class shadows the builtin ----------------------------

BEZEL_TEST(user_class_shadows_builtin_5272, convert_out_int_does_not_run_builtin) {
    // Pre-fix: ASan stack-buffer-overflow, WRITE of size 8. Post-fix: the
    // user's method runs, `d` is the 41 it wrote, and `d + 1` is 42.
    expectOutAsan(kUserConvert,
                  "var ok := Convert.TryToDouble(\"x\", out var d); "
                  "Console.WriteLine(d + 1);",
                  "42\n");
}

BEZEL_TEST(user_class_shadows_builtin_5272, convert_returns_user_bool) {
    // The same call's RESULT type: the user method returns `bool` (a distinct
    // builtin arm shape from `Convert.ToInt32`'s int). Pre-fix the builtin
    // `TryToDouble` arm wrote a `double` into the int slot and returned its
    // own success flag, so `ok` was not the user's.
    expectOut(kUserConvert,
              "var ok := Convert.TryToDouble(\"x\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n41\n");
}

BEZEL_TEST(user_class_shadows_builtin_5272, math_sqrt_returns_user_value) {
    // The family row, with NO memory fault pre-fix: a user `Math` whose static
    // `Sqrt` returns 7.0, but `Math.Sqrt(2.0)` printed the builtin
    // 1.4142135623730951 — a silent wrong answer, the strongest form of this
    // bug because nothing crashes.
    expectOut("public class Math public static method Sqrt(d: double): double "
              "return 7.0; end Sqrt; end Math; ",
              "var x := Math.Sqrt(2.0); Console.WriteLine(x);",
              "7\n");
}

// ---- controls: the builtins still lower when NOTHING shadows them ---------
//
// The fix must not disarm the text-keyed arms. These pin that a bare builtin
// receiver (no user class of that name anywhere in the module) is unchanged —
// without them the fix could pass by simply never reaching a builtin arm.

BEZEL_TEST(user_class_shadows_builtin_5272, builtin_convert_still_lowers) {
    expectOut("",
              "var ok := Convert.TryToDouble(\"2.5\", out var d); "
              "Console.WriteLine(ok); Console.WriteLine(d);",
              "True\n2.5\n");
}

BEZEL_TEST(user_class_shadows_builtin_5272, builtin_convert_to_int32_still_lowers) {
    expectOut("", "Console.WriteLine(Convert.ToInt32(\"42\"));", "42\n");
}

BEZEL_TEST(user_class_shadows_builtin_5272, builtin_math_sqrt_still_lowers) {
    expectOut("", "Console.WriteLine(Math.Sqrt(2.0));", "1.4142135623730951\n");
}

// A user class whose name is NOT a builtin, calling a method the builtin's arm
// ladder would also match by name. This is the no-regression control for the
// #5263 fix that the Convert arm shares a name ladder with: the user's `out`
// slot must still be sized from THEIR declaration.
BEZEL_TEST(user_class_shadows_builtin_5272, non_builtin_name_unaffected) {
    expectOut("public class Helper public static method "
              "TryToDouble(s: string, out r: int): bool r := 41; "
              "return true; end TryToDouble; end Helper; ",
              "var ok := Helper.TryToDouble(\"x\", out var d); "
              "Console.WriteLine(d + 1);",
              "42\n");
}
