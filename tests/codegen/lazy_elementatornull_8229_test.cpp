// Codegen -- stdlib section 5 (v0.355) `ElementAtOrNull(i)`, spec section 15.1
// (v0.355), issue 8229: `xs.Where(pred).ElementAtOrNull(i)` (and the other lazy
// receivers Select / OrderBy / Skip) compiled with the #605 / #2477 / #2434
// internal warnings and printed 0: `ElementAtOrNull` was missing from
// `isArrTerminal` in `Emitter::tryLowerCollectionInstanceCall`
// (src/codegen/LlvmBackend_Calls.cpp), so no lowering arm claimed the call and
// the local was zero-initialised.
//
// Semantics (stdlib section 5): `ElementAtOrNull(i)` returns the element at
// index `i` as `T?`, and null when `i` is out of range (negative or >= count),
// where `ElementAt(i)` would throw.
//
// Every row goes through runAotChecked, so the internal-degrade warnings are
// fatal here and a leak fails the row.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

void expectOut(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

void expectNoLeak(const JitResult& r, const char* out) {
    expectOut(r, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs << " allocation(s) / "
                  << leakedBytes << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(sawSummary ? leakedAllocs : 0ull, 0ull);
    BEZEL_EXPECT_EQ(sawSummary ? leakedBytes : 0ull, 0ull);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

std::string prog(const std::string& body) {
    return "module M; import System; import System.Collections.Generic;\n"
           "record Pt(X: int, Y: int);\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    using var xs := new Array<int>();\n"
           "    xs.Add(10); xs.Add(20); xs.Add(30);\n" +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

}  // namespace

// THE ISSUE'S REPRO, verbatim (probe lz_val.bzl). Expected `30` then `-1`.
BEZEL_TEST(lazy_elementatornull_8229, issue_repro_verbatim_stdlib_5) {
    expectOut(runAotChecked(R"BZL(
module M;
class C public static method Main(): int
  using var xs := new Array<int>();
  xs.Add(10); xs.Add(20); xs.Add(30);
  var v := xs.Where(lambda(x) => x > 10).ElementAtOrNull(1);
  var w := xs.Where(lambda(x) => x > 10).ElementAtOrNull(5);
  Console.WriteLine(v ?? -1);
  Console.WriteLine(w ?? -1);
  return 0;
end Main;
end C;
)BZL"),
              "30\n-1\n");
}

BEZEL_TEST(lazy_elementatornull_8229, where_in_range_first_and_last) {
    expectOut(runAotChecked(prog(
        "    var a := xs.Where(lambda(x) => x > 10).ElementAtOrNull(0);\n"
        "    var b := xs.Where(lambda(x) => x > 10).ElementAtOrNull(1);\n"
        "    Console.WriteLine(a ?? -1);\n"
        "    Console.WriteLine(b ?? -1);")),
        "20\n30\n");
}

// Out of range: index == count, far past, and negative -> null (not a throw).
BEZEL_TEST(lazy_elementatornull_8229, where_out_of_range_and_negative_is_null) {
    expectOut(runAotChecked(prog(
        "    var a := xs.Where(lambda(x) => x > 10).ElementAtOrNull(2);\n"
        "    var b := xs.Where(lambda(x) => x > 10).ElementAtOrNull(-1);\n"
        "    var c := xs.Where(lambda(x) => x > 100).ElementAtOrNull(0);\n"
        "    Console.WriteLine(a = null);\n"
        "    Console.WriteLine(b = null);\n"
        "    Console.WriteLine(c = null);")),
        "True\nTrue\nTrue\n");
}

BEZEL_TEST(lazy_elementatornull_8229, select_orderby_skip_receivers) {
    expectOut(runAotChecked(prog(
        "    var s := xs.Select(lambda(x) => x * 2).ElementAtOrNull(2);\n"
        "    var so := xs.Select(lambda(x) => x * 2).ElementAtOrNull(3);\n"
        "    var o := xs.OrderBy(lambda(x) => 0 - x).ElementAtOrNull(0);\n"
        "    var oo := xs.OrderBy(lambda(x) => 0 - x).ElementAtOrNull(3);\n"
        "    var k := xs.Skip(1).ElementAtOrNull(1);\n"
        "    var ko := xs.Skip(1).ElementAtOrNull(2);\n"
        "    Console.WriteLine(s ?? -1);\n"
        "    Console.WriteLine(so ?? -1);\n"
        "    Console.WriteLine(o ?? -1);\n"
        "    Console.WriteLine(oo ?? -1);\n"
        "    Console.WriteLine(k ?? -1);\n"
        "    Console.WriteLine(ko ?? -1);")),
        "60\n-1\n30\n-1\n30\n-1\n");
}

// A `??` consumer directly on the call, no intermediate local.
BEZEL_TEST(lazy_elementatornull_8229, coalesce_consumer_inline) {
    expectOut(runAotChecked(prog(
        "    Console.WriteLine(xs.Where(lambda(x) => x > 10).ElementAtOrNull(1) ?? -1);\n"
        "    Console.WriteLine(xs.Where(lambda(x) => x > 10).ElementAtOrNull(9) ?? -1);")),
        "30\n-1\n");
}

BEZEL_TEST(lazy_elementatornull_8229, string_elements) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new Array<string>();
    xs.Add("a"); xs.Add("bb"); xs.Add("ccc");
    var v := xs.Where(lambda(s) => s.Length > 1).ElementAtOrNull(1);
    var w := xs.Where(lambda(s) => s.Length > 1).ElementAtOrNull(2);
    Console.WriteLine(v ?? "none");
    Console.WriteLine(w ?? "none");
    return 0;
  end Main;
end C;
)BZL"),
              "ccc\nnone\n");
}

BEZEL_TEST(lazy_elementatornull_8229, record_elements) {
    expectOut(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
record Pt(X: int, Y: int);
class C
  public static method Main(): int
    using var xs := new Array<Pt>();
    xs.Add(new Pt(1, 2)); xs.Add(new Pt(3, 4)); xs.Add(new Pt(5, 6));
    var v := xs.Where(lambda(p) => p.X > 1).ElementAtOrNull(1);
    var w := xs.Where(lambda(p) => p.X > 1).ElementAtOrNull(2);
    Console.WriteLine(v!.Y);
    Console.WriteLine(w = null);
    return 0;
  end Main;
end C;
)BZL"),
              "6\nTrue\n");
}

// 100x loop, in range and out of range, int and string: no leak of the
// materialised snapshot (stdlib section 5 / spec section 28.3).
BEZEL_TEST(lazy_elementatornull_8229, loop_100_no_leak) {
    expectNoLeak(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class C
  public static method Main(): int
    using var xs := new Array<int>();
    xs.Add(10); xs.Add(20); xs.Add(30);
    using var ss := new Array<string>();
    ss.Add("a"); ss.Add("bb"); ss.Add("ccc");
    var total := 0;
    for var i in 0..<100 do
      total := total + (xs.Where(lambda(x) => x > 10).ElementAtOrNull(1) ?? -1);
      total := total + (xs.Where(lambda(x) => x > 10).ElementAtOrNull(7) ?? -1);
      var t := ss.Where(lambda(s) => s.Length > 1).ElementAtOrNull(0);
      if t <> null then total := total + 1; end if;
      var u := ss.Select(lambda(s) => s).ElementAtOrNull(9);
      if u = null then total := total + 1; end if;
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL"),
                 "3100\n");
}
