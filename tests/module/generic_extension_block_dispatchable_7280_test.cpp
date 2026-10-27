// fix #7280 (§26.4 / §26.5 / §26.5.1 / §2.8.2, v0.344) — a DISPATCHABLE
// (ground-head) extension block must not DROP its generic member on export:
// cross-module, the importer used to bind the LESS SPECIFIC block's member
// and print the wrong value (exit 0).
//
// THE DEFECT. A library module declares
//
//     public extension Object public method Foo(x: int): int => 1; end;
//     public extension Plain  public method Foo<X>(x: X): int => 2; end;
//
// and an importer calls `b.Foo(1)` on a `Plain`. §26.5.1 ranks the APPLICABLE
// candidates by the specificity of their receiver-substituted heads: `Plain`
// is a subtype of `Object`, so the `Plain` block is more specific and WINS.
// The same-module program therefore prints 2.
//
// Cross-module, the `Plain` block was carried with NO MEMBERS AT ALL: the
// exporter's method branch skipped every GENERIC member (a generic extension
// method has no carryable body), and `convertExtension`'s
// `return !out.members.empty()` then DROPPED the whole block. The importer
// saw only the exporting module's LESS SPECIFIC `Object` block, bound THAT
// one, printed 1 and exited 0 — a silent wrong answer across a module
// boundary (the #6966 failure mode, in its dispatchable spelling: #7223
// closed the resolution-only one, where the head binds a free type variable).
//
// §2.8.2 ("No restrictions beyond visibility"): "An implementation unable to
// carry some body shape in its interface format must reject the export, or
// the import, with a diagnostic naming the shape; it must not alter the
// instantiation's semantics." The generic method cannot be carried WITH A
// BODY (importer-side monomorphization of an extension block does not exist),
// and it must not be DISPATCHED either: the exporter emits ONE symbol per
// extension member NAME (`bz$ext$<module>.<Target>$<M>`) compiled against
// the exporting module's PLACEHOLDER layout, so the importer would call it
// with the concrete layout. MEASURED by carrying it anyway: an int-typed
// type argument "works" only by ABI luck (printed 7), and a record-typed one
// fails the LLVM verifier ("Call parameter type does not match function
// signature!" — the placeholder pointer against the concrete `%record`).
//
// So the member rides the wire as a SHAPE (its name, arity and type
// parameters — the three things the importer needs to RANK the block and to
// REFUSE the call naming the shape), exactly as #6966's resolution-only
// carriage carries a resolution-only block's ordinary members; and the
// importer REFUSES that member. The refusal is PER MEMBER: the dispatchable
// block's ORDINARY members carry a CONCRETE layout and keep dispatching.
//
// WHY THE ROWS BELOW ARE EIGHT. Five REJECT rows (the defect itself, its
// three spellings, and the same-name spelling — one block declaring a member
// name twice, the generic declaration first) and three ACCEPT CONTROLS that
// pin that the fix does not over-reject — §26.5.1 ranks only the APPLICABLE
// candidates, and a dispatchable block's ordinary members are dispatchable.
// The ACCEPT controls are green on BOTH arms by construction — they pin the
// absence of a regression, not the fix.
//
// The oracle is `runAotModules` — the strict AOT path (fork-isolated
// Driver::compileFile + clang link + native exec), never the deprecated
// in-process JIT harness.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::diagnosticMessageContains;

namespace {

// The library's shared declarations. `Plain` is the receiver type the
// dispatchable block extends; `Derived` is the unrelated shape the ACCEPT
// control uses.
const char* kTypes =
    "public class Plain public constructor() end constructor; end Plain;\n"
    "public class Derived public constructor() end constructor; end Derived;\n";

// A cross-module program: `lib` is compiled as a library artifact, `app` is
// the importer.
JitResult xmod(const std::string& lib, const std::string& app) {
    return bezel::test::runAotModules(
        {ModuleSource{"L", "module L;\nimport System;\n" + lib},
         ModuleSource{"A", "module A;\nimport System;\nimport L;\n" + app}},
        "A");
}

// The SAME-MODULE control: one module holding both the library's declarations
// and the call. It is the answer the importer must agree with (§2.8.2 clause
// 2: an external instantiation "behaves identically to a same-module
// instantiation").
JitResult same(const std::string& lib, const std::string& app) {
    return bezel::test::runAotModules(
        {ModuleSource{"A", "module A;\nimport System;\n" + lib + "\n" + app}},
        "A");
}

// The importer must print `want` — the SAME value the same-module program
// prints (the ACCEPT rows).
void expectXmodPrints(const std::string& lib, const std::string& app,
                       const char* want) {
    JitResult r = xmod(lib, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// The importer must be REJECTED, and the diagnostic must NAME THE SHAPE —
// §2.8.2's "reject the export, or the import, with a diagnostic naming the
// shape". The shape named is the block whose member is generic, together
// with the module and the site.
void expectXmodRejectsNamingShape(const std::string& lib,
                                   const std::string& app,
                                   const char* shape) {
    JitResult r = xmod(lib, app);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, shape));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§2.8.2"));
}

// The importer's program text: a `Plain` receiver calling `Foo(1)`.
const char* kPlainFooApp =
    "class P public static method Main(): int "
    "  var b := new Plain(); "
    "  Console.WriteLine($\"{b.Foo(1)}\"); "
    "  return 0; end Main; end P;\n";

}  // namespace

// ---------------------------------------------------------------------------
// The REJECT rows — the defect itself.
// ---------------------------------------------------------------------------

// The issue's own shape: the dispatchable `Plain` block out-ranks the
// `Object` block for a `Plain` receiver (§26.5.1), so the importer must not
// answer with the `Object` block's 1. It must reject, naming
// `extension Plain`.
//
// FAILS WITHOUT THE FIX: the importer binds the `Object` block, prints 1 and
// exits 0 — no diagnostic at all.
BEZEL_TEST(xmod_extension_generic_member_dispatchable_7280,
           dispatchable_head_generic_only_member_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Plain public method Foo<X>(x: X): int => 2; end;\n";
    // The same-module program binds the `Plain` block and prints 2 — the
    // answer the importer must not contradict.
    JitResult s = same(lib, kPlainFooApp);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, kPlainFooApp, "extension Plain");
}

// The generic member is not the block's ONLY member: the block also declares
// an ordinary `Bar`, so the block WAS carried (with `Bar` alone) — but the
// generic member's NAME was missing from the wire, so the block was not a
// candidate for the `Foo` call and the less specific `Object` block answered.
//
// FAILS WITHOUT THE FIX: the importer binds the `Object` block, prints 1 and
// exits 0.
BEZEL_TEST(xmod_extension_generic_member_dispatchable_7280,
           dispatchable_head_generic_member_beside_plain_member_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Plain public method Foo<X>(x: X): int => 2; "
        "  public method Bar(): int => 3; end;\n";
    JitResult s = same(lib, kPlainFooApp);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, kPlainFooApp, "extension Plain");
}

// The VALUE-TYPED type argument — the spelling that fails the LLVM verifier
// when the member is carried WITH a body and dispatched: the exporter's one
// symbol per member name is compiled against the exporting module's
// placeholder layout, and the importer would call it with the concrete
// `%record` layout ("Call parameter type does not match function
// signature!"). The importer must refuse instead.
//
// FAILS WITHOUT THE FIX, AND NOT BY PRINTING 1: on the base arm this call was
// REJECTED with a DIFFERENT diagnostic — "the call to `Foo`: argument 1 has
// a type that is not implicitly convertible to the target type (§5.1)" —
// because the generic member was not on the wire, so the importer bound the
// `Object` block's `Foo(x: int)`, whose parameter a record does not fit. The
// row still discriminates: the §2.8.2 diagnostic that NAMES THE SHAPE is
// absent on the base arm, and present with the fix.
BEZEL_TEST(xmod_extension_generic_member_dispatchable_7280,
           dispatchable_head_record_typed_type_argument_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public record Tri(A: int, B: int);\n"
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Plain public method Foo<X>(x: X): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var b := new Plain(); "
        "  var t := new Tri(1, 2); "
        "  Console.WriteLine($\"{b.Foo(t)}\"); "
        "  return 0; end Main; end P;\n";
    JitResult s = same(lib, app);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, app, "extension Plain");
}

// The EXPLICIT type-argument call (`b.Foo<int>(1)`) names the generic member
// directly. The importer cannot monomorphize it either, so the call must be
// refused naming the shape — not answered with the `Object` block's 1.
//
// FAILS WITHOUT THE FIX, AND NOT BY PRINTING 1: on the base arm this call was
// REJECTED with a DIFFERENT diagnostic — "call to `Foo` supplies 1 explicit
// type argument(s), but `Foo` declares 0 type parameter(s) — an explicit
// type-argument list must supply exactly as many arguments as the method
// declares type parameters; there are no partial lists (§14.5)" — because the
// generic member was not on the wire, so the importer saw a `Foo` that
// declares no type parameters at all. The row still discriminates: the
// §2.8.2 diagnostic that NAMES THE SHAPE is absent on the base arm, and
// present with the fix.
BEZEL_TEST(xmod_extension_generic_member_dispatchable_7280,
           explicit_type_argument_call_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Plain public method Foo<X>(x: X): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var b := new Plain(); "
        "  Console.WriteLine($\"{b.Foo<int>(1)}\"); "
        "  return 0; end Main; end P;\n";
    JitResult s = same(lib, app);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, app, "extension Plain");
}

// ONE MEMBER NAME DECLARED TWICE IN ONE BLOCK, THE GENERIC DECLARATION
// FIRST. The exporter emits ONE symbol per extension member NAME, so the
// symbol an importer calls for `Foo` is the FIRST declaration's — the
// generic member's, compiled against the placeholder layout. On the base arm
// the generic member was not on the wire at all, so the importer bound the
// ordinary member and called that symbol anyway: cross-module printed 2 (the
// generic member's placeholder body) while the same-module program printed 3
// (the ordinary member, which §22.6's ranking picks), exit 0 — a silent
// wrong answer. The fix refuses the call naming the shape: dispatching
// either member here would call the generic member's placeholder symbol.
//
// FAILS WITHOUT THE FIX: the importer prints 2 and exits 0 — no diagnostic
// at all.
BEZEL_TEST(xmod_extension_generic_member_dispatchable_7280,
           same_name_generic_declared_first_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Plain "
        "  public method Foo<X>(x: X): int => 2; "
        "  public method Foo(x: int): int => 3; end;\n";
    const char* app = kPlainFooApp;
    // The same-module program ranks the two `Foo` declarations and picks the
    // ordinary one (§22.6), printing 3 — the answer the importer must not
    // contradict with 2.
    JitResult s = same(lib, app);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "3\n");

    expectXmodRejectsNamingShape(lib, app, "extension Plain");
}

// ---------------------------------------------------------------------------
// The ACCEPT CONTROLS — the fix must not over-reject.
// ---------------------------------------------------------------------------

// The dispatchable block's ORDINARY member still dispatches: it is exported
// with a CONCRETE layout, so the importer binds and calls it normally. The
// refusal is PER MEMBER (§26.3), not per block.
//
// Green on BOTH arms: it pins the absence of a regression.
BEZEL_TEST(xmod_extension_generic_member_dispatchable_7280,
           ordinary_member_of_dispatchable_block_still_dispatches_26_3) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Plain public method Foo<X>(x: X): int => 2; "
        "  public method Bar(): int => 3; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var b := new Plain(); "
        "  Console.WriteLine($\"{b.Bar()}\"); "
        "  return 0; end Main; end P;\n";
    expectXmodPrints(lib, app, "3\n");
    JitResult s = same(lib, app);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "3\n");
}

// The receiver is unrelated to the dispatchable block (`Derived` is not a
// `Plain`), so that block is not even a candidate and the `Object` block
// applies. The importer must print 1 — not reject. §26.5.1 ranks only the
// APPLICABLE candidates.
//
// Green on BOTH arms: it pins the absence of a regression.
BEZEL_TEST(xmod_extension_generic_member_dispatchable_7280,
           unrelated_receiver_keeps_exported_member_26_5_1) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Plain public method Foo<X>(x: X): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var d := new Derived(); "
        "  Console.WriteLine($\"{d.Foo(1)}\"); "
        "  return 0; end Main; end P;\n";
    expectXmodPrints(lib, app, "1\n");
    JitResult s = same(lib, app);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "1\n");
}

// The competitor declares a DIFFERENT member name (`Bar`), so it is not a
// candidate for `Foo` at all and the `Object` block answers. The importer
// must print 1.
//
// Green on BOTH arms: it pins the absence of a regression.
BEZEL_TEST(xmod_extension_generic_member_dispatchable_7280,
           different_member_name_keeps_exported_member_26_5) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Plain public method Bar<X>(x: X): int => 2; end;\n";
    expectXmodPrints(lib, kPlainFooApp, "1\n");
    JitResult s = same(lib, kPlainFooApp);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "1\n");
}
