// fix #7223 (§26.4 / §26.5 / §26.5.1 / §2.8.2, v0.344) — a cross-module
// extension call must not silently bind a LESS SPECIFIC block's member when
// the MORE SPECIFIC block's only visible member is a GENERIC method.
//
// THE DEFECT. A library module declares
//
//     public extension Object    { public method Foo(x: int): int => 1; }
//     public extension Box<T>    { public method Foo<X>(x: X): int => 2; }
//
// and an importer calls `b.Foo(1)` on a `Box<int>`. §26.5.1 ranks the
// APPLICABLE candidates by the specificity of their receiver-substituted
// heads: `Box<int>` is a subtype of `Object`, so the `Box<T>` block is more
// specific and WINS. The same-module program therefore prints 2.
//
// Cross-module, the `Box<T>` block was carried with NO MEMBERS AT ALL, so it
// never reached the wire: the exporter's method branch skipped every GENERIC
// member (a generic extension method has no carryable body), and
// `convertExtension`'s `return !out.members.empty()` then DROPPED the whole
// block. The importer saw only the exporting module's LESS SPECIFIC
// `Object` block, bound THAT one, printed 1 and exited 0 — a silent wrong
// answer across a module boundary.
//
// §2.8.2 ("No restrictions beyond visibility"): "An implementation unable to
// carry some body shape in its interface format must reject the export, or
// the import, with a diagnostic naming the shape; it must not alter the
// instantiation's semantics." The generic method cannot be carried WITH A
// BODY (importer-side monomorphization of an extension block does not exist),
// but it CAN be carried as a SHAPE — the member's NAME and ARITY, which is
// exactly what #6966's resolution-only carriage carries for the ordinary
// members of a free-variable / `where`-clause block. With the shape carried,
// the importer SEES the block, ranks it (it is the more specific one), and
// refuses the call naming the shape — instead of answering a different
// question with the `Object` block's 1.
//
// WHY THE ROWS BELOW ARE SEVEN. Four REJECT rows (the defect itself and its
// three spellings), two ACCEPT CONTROLS that pin that the fix does not
// over-reject (§26.5.1 ranks only the APPLICABLE candidates), and one row for
// the EXPLICIT type-argument call, which must be refused too. The ACCEPT
// controls are green on BOTH arms by construction — they pin the absence of a
// regression, not the fix.
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

// The library's shared declarations. `Box<T>` is the receiver type; `Derived`
// is the unrelated shape the ACCEPT controls use.
const char* kTypes =
    "public class Box<T> public v: T; "
    "  public constructor(a: T) v := a; end constructor; end Box;\n"
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
// shape". The shape named is the block whose head binds the free type
// variable (`extension Box<T>`), together with the module and the site.
void expectXmodRejectsNamingShape(const std::string& lib,
                                   const std::string& app,
                                   const char* shape) {
    JitResult r = xmod(lib, app);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, shape));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§2.8.2"));
}

// The importer's program text: a `Box<int>` receiver calling `Foo(1)`.
const char* kBoxFooApp =
    "class P public static method Main(): int "
    "  var b := new Box<int>(1); "
    "  Console.WriteLine($\"{b.Foo(1)}\"); "
    "  return 0; end Main; end P;\n";

}  // namespace

// ---------------------------------------------------------------------------
// The REJECT rows — the defect itself.
// ---------------------------------------------------------------------------

// The issue's own shape: the `Box<T>` block out-ranks the `Object` block for
// a `Box<int>` receiver (§26.5.1), so the importer must not answer with the
// `Object` block's 1. It must reject, naming `extension Box<T>`.
//
// FAILS WITHOUT THE FIX: the importer binds the `Object` block, prints 1 and
// exits 0 — no diagnostic at all.
BEZEL_TEST(xmod_extension_generic_method_7223,
           free_variable_head_generic_only_member_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Box<T> public method Foo<X>(x: X): int => 2; end;\n";
    // The same-module program binds the `Box<T>` block and prints 2 — the
    // answer the importer must not contradict.
    JitResult s = same(lib, kBoxFooApp);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, kBoxFooApp, "extension Box<T>");
}

// The `where`-clause sibling of the same shape: a `where` clause is the OTHER
// reason §26.4 calls a block resolution-only, and the resolution-only
// carriage is the same. The importer must reject, naming the block's head
// INCLUDING its `where` clause.
//
// FAILS WITHOUT THE FIX: the importer binds the `Object` block, prints 1 and
// exits 0.
BEZEL_TEST(xmod_extension_generic_method_7223,
           constrained_free_variable_head_generic_only_member_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Box<T> where T: IComparable<T> "
        "  public method Foo<X>(x: X): int => 2; end;\n";
    JitResult s = same(lib, kBoxFooApp);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, kBoxFooApp, "extension Box<T>");
}

// The block ALONE: with no `Object` competitor there is no less specific
// member to bind wrongly, but the importer still cannot HONOUR the shape (no
// importer-side monomorphization for an extension block, §26.4), so it must
// refuse — and the refusal must NAME THE SHAPE, which is what §2.8.2 asks
// for.
//
// FAILS WITHOUT THE FIX: the importer refuses with the plain
// "no member `Foo` on type `Box` (§11.3, §12.1)" wording, which names no
// shape and no §2.8.2.
BEZEL_TEST(xmod_extension_generic_method_7223,
           free_variable_head_generic_only_member_alone_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Box<T> public method Foo<X>(x: X): int => 2; end;\n";
    JitResult s = same(lib, kBoxFooApp);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, kBoxFooApp, "extension Box<T>");
}

// The block's generic member is not its ONLY member: the block also declares
// an ordinary `Bar`, so the block WAS carried (with `Bar` alone) — but the
// generic member's NAME was missing from the wire, so the block was not a
// candidate for the `Foo` call and the less specific `Object` block answered.
//
// FAILS WITHOUT THE FIX: the importer binds the `Object` block, prints 1 and
// exits 0.
BEZEL_TEST(xmod_extension_generic_method_7223,
           free_variable_head_generic_member_beside_plain_member_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Box<T> public method Foo<X>(x: X): int => 2; "
        "  public method Bar(): int => 3; end;\n";
    JitResult s = same(lib, kBoxFooApp);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, kBoxFooApp, "extension Box<T>");
}

// The EXPLICIT type-argument call (`b.Foo<int>(1)`) names the generic member
// directly. The importer cannot monomorphize it either, so the call must be
// refused naming the shape — not answered with the `Object` block's 1.
//
// FAILS WITHOUT THE FIX: the importer binds the `Object` block, prints 1 and
// exits 0.
BEZEL_TEST(xmod_extension_generic_method_7223,
           explicit_type_argument_call_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Box<T> public method Foo<X>(x: X): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var b := new Box<int>(1); "
        "  Console.WriteLine($\"{b.Foo<int>(1)}\"); "
        "  return 0; end Main; end P;\n";
    JitResult s = same(lib, app);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, app, "extension Box<T>");
}

// ---------------------------------------------------------------------------
// The ACCEPT CONTROLS — the fix must not over-reject. §26.5.1 ranks only the
// APPLICABLE candidates, so a block that does not apply to the receiver
// cannot make the importer refuse.
//
// Both rows are green on BOTH arms: they pin the absence of a regression.
// ---------------------------------------------------------------------------

// The receiver is unrelated to the `Box<T>` competitor (`Derived` is not a
// `Box`), so the competitor is not even a candidate and the `Object` block
// applies. The importer must print 1 — not reject.
BEZEL_TEST(xmod_extension_generic_method_7223,
           unrelated_receiver_keeps_exported_member_26_5_1) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Box<T> public method Foo<X>(x: X): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var d := new Derived(); "
        "  Console.WriteLine($\"{d.Foo(1)}\"); "
        "  return 0; end Main; end P;\n";
    expectXmodPrints(lib, app, "1\n");
}

// The competitor declares a DIFFERENT member name (`Bar`), so it is not a
// candidate for `Foo` at all and the `Object` block answers. The importer
// must print 1.
BEZEL_TEST(xmod_extension_generic_method_7223,
           different_member_name_keeps_exported_member_26_5) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(x: int): int => 1; end;\n"
        "public extension Box<T> public method Bar<X>(x: X): int => 2; end;\n";
    expectXmodPrints(lib, kBoxFooApp, "1\n");
}
