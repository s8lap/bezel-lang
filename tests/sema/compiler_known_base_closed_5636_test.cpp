// fix #5636 / spec §12.4 *Compiler-known classes are closed to inheritance unless
// their text opens them* (v0.338): a base-type reference in a user class to a
// compiler-known class (a class of the standard library, or a built-in exception
// type) is a compile error unless the text that declares the class opens it to
// user subclasses. The opened set is Encoding, Stream, TextReader, TextWriter
// (stdlib §1 *Extensibility*) and the built-in exception family (§31.4). Every
// other compiler-known class is closed exactly as a `sealed` class is. `Object`
// is not reached by the rule.
//
// Before the fix a built-in base resolved with no user TypeSymbol and was
// recorded only in `nonSymbolBaseRefs`; the `sealed` check reached declared bases
// only, so `class E: Mutex end E;` and the other closed shapes compiled.
//
// Every reject row asserts the §12.4 needle and the compile-time rejection
// (never `hasErrors()` alone). The accept controls assert `r.ran` and the exit
// code (a program that must still accept must RUN).

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A module whose only class derives from `base` (the base-list text, verbatim),
// plus a Main so an accepted program runs and exits 0.
std::string programDeriving(const std::string& declaration) {
    return "module M; " + declaration + " "
           "class C public static method Main(): int return 0; end Main; end C;";
}

}  // namespace

BEZEL_TEST(sema_compiler_known_base_5636, mutex_base_rejected_sec_12_4) {
    // §12.4 (v0.338): `Mutex` is a compiler-known library class its text does not
    // open; a user class may not name it as its base.
    auto r = runAot(programDeriving("class E: Mutex end E;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot inherit from compiler-known class `Mutex`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "closed to inheritance"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""12.4"));
}

BEZEL_TEST(sema_compiler_known_base_5636, stringbuilder_base_rejected_sec_12_4) {
    // §12.4 (v0.338): `StringBuilder` is closed; `StringBuilder` is not in the
    // opened set.
    auto r = runAot(programDeriving("class B: StringBuilder end B;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot inherit from compiler-known class `StringBuilder`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""12.4"));
}

BEZEL_TEST(sema_compiler_known_base_5636, generic_array_base_rejected_sec_12_4) {
    // §12.4 (v0.338): a generic compiler-known class reached through its type
    // arguments is still the closed class `Array`.
    auto r = runAot(programDeriving("class A: Array<int> end A;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot inherit from compiler-known class `Array`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""12.4"));
}

BEZEL_TEST(sema_compiler_known_base_5636, filestream_base_rejected_sec_12_4) {
    // §12.4 (v0.338): `FileStream` derives from the opened `Stream`, but its own
    // section does not open it; the closed-class rule is per class.
    auto r = runAot(programDeriving("class F: FileStream end F;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot inherit from compiler-known class `FileStream`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""12.4"));
}

BEZEL_TEST(sema_compiler_known_base_5636, dictionary_base_rejected_sec_12_4) {
    // §12.4 (v0.338): a closed generic collection named with type arguments.
    auto r = runAot(programDeriving("class D: Dictionary<int, int> end D;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot inherit from compiler-known class `Dictionary`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""12.4"));
}

BEZEL_TEST(sema_compiler_known_base_5636, task_base_rejected_sec_12_4) {
    // §12.4 (v0.338): `Task` is a compiler-known class its section does not open.
    auto r = runAot(programDeriving("class T2: Task<int> end T2;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot inherit from compiler-known class `Task`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""12.4"));
}

BEZEL_TEST(sema_compiler_known_base_5636, owning_task_base_rejected_sec_12_4) {
    // §12.4 (v0.338): `OwningTask<T>` is declared `public class OwningTask<T>: Task`
    // (spec §30.3, v0.338); its text opens nothing, so it is closed like `Task`.
    auto r = runAot(programDeriving("class OT: OwningTask<int> end OT;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot inherit from compiler-known class `OwningTask`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""12.4"));
}

BEZEL_TEST(sema_compiler_known_base_5636, cancellation_source_base_rejected_sec_12_4) {
    // §12.4 (v0.338): `CancellationTokenSource` is closed (stdlib §6.1 does not
    // open it); the import is spelled so the name resolves.
    auto r = runAot("module M; import System.Threading; "
                    "class X: CancellationTokenSource end X; "
                    "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot inherit from compiler-known class `CancellationTokenSource`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""12.4"));
}

BEZEL_TEST(sema_compiler_known_base_5636, opened_stream_base_admitted_sec_12_4) {
    // §12.4 (v0.338) control: `Stream`'s listing opens it (stdlib §1
    // *Extensibility*, §7.1), so a user subclass is admitted.
    auto r = runAot(programDeriving("abstract class MyS: Stream end MyS;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_compiler_known_base_5636, opened_encoding_base_admitted_sec_12_4) {
    // §12.4 (v0.338) control: `Encoding` is opened (stdlib §3.2).
    auto r = runAot(programDeriving("abstract class MyE: Encoding end MyE;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_compiler_known_base_5636, opened_textwriter_base_admitted_sec_12_4) {
    // §12.4 (v0.338) control: `TextWriter` is opened (stdlib §7.4).
    auto r = runAot(programDeriving("abstract class MyW: TextWriter end MyW;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_compiler_known_base_5636, exception_base_admitted_sec_12_4) {
    // §12.4 (v0.338) control: the built-in exception family is opened (§31.4).
    auto r = runAot(programDeriving("class MyError: Exception end MyError;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_compiler_known_base_5636, builtin_io_exception_base_admitted_sec_12_4) {
    // §12.4 (v0.338) control: a built-in exception type of §31.4's family in its
    // own module (`System.IO`) is opened.
    auto r = runAot(programDeriving("class MyIo: IOException end MyIo;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_compiler_known_base_5636, object_base_admitted_sec_12_4) {
    // §12.4 (v0.338) control: `Object` is every class's implicit base and is not
    // reached by the rule.
    auto r = runAot(programDeriving("class O: Object end O;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_compiler_known_base_5636, user_generic_base_admitted_sec_12_4) {
    // §12.4 (v0.338) control: a user generic base is not compiler-known.
    auto r = runAot(programDeriving(
        "class G<T> end G; class H: G<int> end H;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_compiler_known_base_5636, interface_base_admitted_sec_12_4) {
    // §12.4 (v0.338) control: an interface in the base list is not a class and
    // is outside this rule.
    auto r = runAot(programDeriving(
        "interface IShape method Area(): int; end IShape; "
        "class W: IShape public method Area(): int return 1; end Area; end W;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_compiler_known_base_5636, contract_violation_exception_base_admitted_sec_12_4) {
    // §12.4 (v0.338) control: ContractViolationException joins the built-in
    // exception hierarchy (§31.4), so it is an opened class.
    auto r = runAot(programDeriving("class CV: ContractViolationException end CV;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
