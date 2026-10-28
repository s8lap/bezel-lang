// Spec-derived regression test for issue #7449.
//
// Spec §26.5 rule 1 (v0.345, docs/Bezel_Language_Spec.md:5714): "If `x`'s
// declared type (or any base type) *declares* any instance member named
// `Foo` — of any arity, applicability, or accessibility — instance
// resolution is committed and extension blocks are **never consulted** for
// that name on that receiver."
//
// Stdlib §4.1 (v0.345) declares `pure readonly property Count: int` on
// `IReadOnlyCollection<out T>`, and `IList<T>` INHERITS it through
// `IReadOnlyList<T>` — so on an `IList<int>` receiver the name `Count` is
// an INHERITED instance member, and rule 1 commits instance resolution for
// it. The user's extension is therefore unreachable, and the method
// reference `c.Count` must not be formed from it.
//
// THE DEFECT — MEMORY-UNSAFE. A method reference to an `IList<T>` member,
// formed with an INFERRED target type, under a same-named user extension,
// SEGFAULTED: an 8-byte read past a 4-byte slot (ASan stack-buffer-
// overflow, in the generated `main`).
//
//   extension IList<int> public method Count(): int => 77; end;
//   var c: IList<int> := a;
//   var f := c.Count;
//   Console.WriteLine("FORMED");   // printed
//   Console.WriteLine(f());        // SIGSEGV (rc=139, core dumped)
//
// THE ISOLATION — the extension is what turns a benign degradation into a
// memory-unsafe one:
//
//   with the extension    -> rc=139 (core dumped); ASan stack-buffer-overflow
//   without the extension -> rc=0, prints "FORMED", and the call lowers to
//                           nothing (#3079's degradation)
//
// The EXPLICIT-target-type spelling (`var f: () -> int := c.Count;`) crashes
// WITH OR WITHOUT the extension: that is issue #7090, which this lane does
// NOT close and which is unaffected by this fix (measured: the ASan
// stack-buffer-overflow is identical on both arms).
//
// THE ORACLE. The oracle is the ASan leg: the program must COMPILE, RUN and
// be CLEAN. The loop runs the reference 200 times — a crash-oracle repro
// that crashes ONCE is not a repro (the house's rule), so the shape is
// looped. The value row pins the program's exact STDOUT as well, so the row
// is not vacuous when the ASan toolchain is unavailable.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The repro: a method reference to an `IList<T>` member, under a same-named
// user extension, called in a LOOP.
std::string program(const std::string& ext) {
    return "module M;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    var c: IList<int> := a;\n"
           "    var f := c.Count;\n"
           "    Console.WriteLine(\"FORMED\");\n"
           "    for var i in 0 ..<200 do\n"
           "      f();\n"
           "    end;\n"
           "    Console.WriteLine(\"DONE\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The user's extension: the same name as the INHERITED `Count`.
const char kCountExtension[] =
    "extension IList<int>\n"
    "  public method Count(): int => 77;\n"
    "end;\n";

}  // namespace

// The DEFECT: the same-named user extension must not make the method
// reference crash. The program compiles, runs, and is ASan-clean.
BEZEL_TEST(codegen_collection_interface_method_reference_7449,
           same_named_extension_reference_is_clean) {
    // per §26.5 rule 1 (v0.345) + stdlib §4.1
    const std::string src = program(kCountExtension);
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "FORMED\nDONE\n");
    // The ASan leg: 0 == the program ran to completion with no sanitizer
    // report. -1 means the toolchain is unavailable (the leg is then
    // skipped, and the stdout assertion above still carries the row).
    int asan = bezel::test::runUnderAsanValue(src, 0, "bezel_7449ext");
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// The CONTROL: with NO extension the program compiled, ran and degraded
// before the fix, and it must still compile, run and be clean after it.
BEZEL_TEST(codegen_collection_interface_method_reference_7449,
           no_extension_reference_is_clean) {
    // per stdlib §4.1 + §27.6
    const std::string src = program("");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "FORMED\nDONE\n");
    int asan = bezel::test::runUnderAsanValue(src, 0, "bezel_7449noext");
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}
