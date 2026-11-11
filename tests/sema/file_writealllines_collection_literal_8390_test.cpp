// fix #8390 — a collection literal passed to `File.WriteAllLines`'s
// `lines: IEnumerable<string>` parameter was ACCEPTED and the call did
// nothing (spec §15.4, v0.357; stdlib §7.3).
//
// WHAT THE DOCUMENTS REQUIRE.
//   spec §15.4 (v0.357): "A collection literal has no standalone type. It is
//   well-typed only in a context that provides the target type", the target
//   list names no interface type, and "A collection literal in a position
//   where no target type is determinable is a compile error".
//   stdlib §7.3: `public static method WriteAllLines(path: string,
//   lines: IEnumerable<string>);` -- the parameter is an INTERFACE, so the
//   literal has no concrete target there.
//
// THE DEFECT (issue repro, measured on the base of this branch): the call
// compiled with no diagnostic and created NO file. The oracle is therefore
// the DIAGNOSTIC (silent acceptance is invisible in an exit code); the
// control pins the legal spelling (a typed `Array<string>` local) by its
// observable output, so a gate that over-rejected would fail too.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module P;\nimport System.IO;\nclass C\n"
           "  public static method Main()\n" + body +
           "\n  end Main;\nend C;\n";
}

bool interfaceTargetRefused(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("cannot target the interface type "
                           "`IEnumerable<string>`") != std::string::npos &&
            d.message.find("15.4") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// The issue's literal repro: the literal at the `lines` position.
BEZEL_TEST(sema_file_writealllines_literal_8390,
           literal_lines_argument_rejected_15_4) {
    auto r = runAot(bezel::test::spliceDataPaths(prog(
        "    const p := \"@DATA@/t1.txt\";\n"
        "    File.WriteAllLines(p, [\"alpha\", \"beta\"]);\n"
        "    Console.WriteLine(File.Exists(p));"), "bzt8390"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(interfaceTargetRefused(r));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// The empty literal is the same refusal (still no target type).
BEZEL_TEST(sema_file_writealllines_literal_8390,
           empty_literal_lines_argument_rejected_15_4) {
    auto r = runAot(bezel::test::spliceDataPaths(prog(
        "    File.WriteAllLines(\"@DATA@/t2.txt\", []);"), "bzt8390"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(interfaceTargetRefused(r));
}

// CONTROL: the remedy the issue names -- a typed local -- still writes the
// file, byte for byte ("q\nr\n" is 4 bytes).
BEZEL_TEST(sema_file_writealllines_literal_8390,
           typed_array_local_still_writes_file_control) {
    auto out = runAotOut(bezel::test::spliceDataPaths(prog(
        "    const p := \"@DATA@/t3.txt\";\n"
        "    var xs: Array<string> := [\"q\", \"r\"];\n"
        "    File.WriteAllLines(p, xs);\n"
        "    Console.Write(File.ReadAllText(p).Length);"), "bzt8390"));
    BEZEL_EXPECT_EQ(out, std::string("4"));
}
