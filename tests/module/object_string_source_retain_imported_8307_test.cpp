// Module — the CROSS-MODULE twin of issue #8307: a `string` handed to an
// IMPORTED method's `Object` formal that the library body stores into a field
// (`Put(o: Object) this.O := o;`). §28.9 (Runtime-managed types), §28.3
// (plain parameters are borrows), §8.6, §2.5 (an imported member has no body
// in the importing module), v0.357.
//
// The library's store of an `Object`-typed source is raw (it may hold a class
// instance or a box, so it cannot be retained as a string), and the importer
// cannot see the body to prove the formal does not escape. Before the fix the
// importer still treated the call as a call-bounded borrow: a named string
// local's scope-end release (the #1430 relaxation) and an unbound +1
// producer's post-call release (#429) both freed the string under the field.
//
// MEASURED on the pre-fix build (r30-c1 e49c32aca), two modules, 200
// iterations: generated-code ASan heap-use-after-free on both shapes.
//
// POST-FIX: an imported callee answers "escapes" (`objectFormalArgNonEscaping`
// refuses an imported member), so the string keeps its +1 — the field's
// reference is the documented #5059 / #8214 residual leak, so these rows run
// with LeakSanitizer OFF and claim only "no use-after-free".

#include <cstdlib>
#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;

namespace {

const char* kLib =
    "module Lib; "
    "public class Holder "
    "  public O: Object := \"\"; "
    "  public method Put(o: Object) "
    "    this.O := o; "
    "  end Put; "
    "end Holder; ";

// `store` runs inside a `do ... end` block whose end releases the string's
// owner; the field is read back after it (and after a fresh allocation).
std::string app(const std::string& store) {
    return "module App; "
           "import Lib; "
           "public class Make "
           "  public static method Str(n: int): string => \"s\" + n.ToString(); "
           "end Make; "
           "public class Program "
           "  public static method Main(): int "
           "    var i := 0; var total := 0; "
           "    while i < 100 do "
           "      var h := new Holder(); "
           "      do " + store + " end; "
           "      var junk := Make.Str(99999); "
           "      total := total + h.O.ToString().Length + junk.Length - "
           "junk.Length; "
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine(total); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The two-module ASan oracle with LeakSanitizer OFF (the two-module twin of
// `runAotCheckedLenient`), the helper object_string_store_retain_imported_8306
// documents: generated-code ASan ON, `detect_leaks=0`, and a missing C
// toolchain reported as a SKIP diagnostic, never absorbed.
JitResult runAotModulesNoLeak(const std::vector<ModuleSource>& modules,
                              const char* entry) {
    static const bool toolchainProbeOk = [] {
        return std::system("clang --version > /dev/null 2>&1") == 0;
    }();
    if (!toolchainProbeOk) {
        JitResult r;
        r.diagnostics.report(
            bezel::Severity::Error, bezel::SourceLocation{},
            "runAotModulesNoLeak: SKIP — C toolchain (clang) unavailable for "
            "the ASan link");
        return r;
    }
    bezel::codegen::CodegenOptions opts;
    opts.checkedContracts = true;
    opts.sanitizeAddress = true;
    return bezel::test::runAotModulesImpl(
        modules, entry, "runAotModulesNoLeak", std::move(opts),
        "detect_leaks=0:exitcode=99:abort_on_error=0");
}

void expectNoUseAfterFree(const std::string& store) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", kLib});
    mods.push_back({"App", app(store)});
    JitResult r = runAotModulesNoLeak(mods, "App");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Sum over i in 0..99 of ("s" + i).Length: 10 * 2 + 90 * 3.
    BEZEL_EXPECT_EQ(r.stdoutText, "290\n");
}

}  // namespace

// A named string local passed to the imported storing `Object` formal.
BEZEL_TEST(object_string_source_retain_imported_8307,
           string_local_to_imported_storing_object_formal_spec_28_9_28_3) {
    expectNoUseAfterFree("var x := Make.Str(i); h.Put(x);");
}

// An unbound +1 producer passed to the same formal.
BEZEL_TEST(object_string_source_retain_imported_8307,
           producer_to_imported_storing_object_formal_spec_28_9_28_3) {
    expectNoUseAfterFree("h.Put(Make.Str(i));");
}

// The issue's q15 spelling across the module boundary.
BEZEL_TEST(object_string_source_retain_imported_8307,
           as_object_to_imported_storing_object_formal_spec_28_9_17_4) {
    expectNoUseAfterFree("var x := Make.Str(i); h.Put(x as Object);");
}
