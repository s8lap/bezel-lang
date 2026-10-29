// Fix #7522 -- a temporary `Array<char>` passed to `string.Trim(chars)` was
// never disposed (stdlib §9.6, §28.3 *Unbound owned temporaries*, §28.9,
// v0.350).
//
// THE DEFECT. stdlib §9.6 declares `Trim(chars: IEnumerable<char>): string`.
// The lowering reads the set and never stores it, but an `owned Array<char>`
// call result (or a `new Array<char>(n)`) handed in as `chars` is an unbound
// +1 temporary: §28.3 owes it a disposal at the full-expression boundary and
// nothing released it (80 B and 2 allocations per call).
//
// THE FIX. After the helper has produced its result, the Trim(chars) arm
// releases the argument through `freeUnboundOwnedBorrowingArrayTemp`, which
// is a no-op for any shape that is not a fresh owned temporary.
//
// SHAPE MATRIX (measured). A bare collection literal as the argument is
// refused by sema (§15.4: a literal cannot target `IEnumerable<char>`), and
// a `Set<char>` is not convertible to `IEnumerable<char>` here; `TrimStart` /
// `TrimEnd` declare no parameter. So the admitted +1 shapes are an
// `owned Array<char>` call result and a `new Array<char>(n)`; a `string`
// producer was already discharged by #7222's funnel (control row).
//
// ORACLE. `runAotChecked` (generated-code ASan + LSan); loops of 200.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& body) {
    return std::string(
               "module M; import System;\nclass C\n"
               "  public static method MkA(): owned Array<char>\n"
               "    return [' ', 'a'];\n"
               "  end MkA;\n"
               "  public static method MkS(): string\n"
               "    return \" a\" + \"\";\n"
               "  end MkS;\n"
               "  public static method Main(): int\n"
               "    var i := 0;\n"
               "    var n := 0;\n") +
           body + "\n    Console.WriteLine(n);\n    return 0;\n  end Main;\nend C;\n";
}

std::string stdoutOf(const JitResult& r) {
    std::string t = r.stdoutText;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    return t;
}

}  // namespace

// §9.6 / §28.3 -- the call-result array is released (the issue's repro).
BEZEL_TEST(string_trim_chars_temp_array_leak_7522, call_result_array_no_leak_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "    while i < 200 do\n"
        "      n := n + \" xa \".Trim(MkA()).Length;\n"
        "      i := i + 1;\n"
        "    end while;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "200");
}

// §28.3 -- the `new` face of the unbound owned temporary.
BEZEL_TEST(string_trim_chars_temp_array_leak_7522, new_array_temp_no_leak_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "    while i < 200 do\n"
        "      n := n + \" xa \".Trim(new Array<char>(2)).Length;\n"
        "      i := i + 1;\n"
        "    end while;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "800");
}

// §28.9 -- a BORROWED local array is not released by the call (no double
// free): it is used again after the loop and disposed once at scope end.
BEZEL_TEST(string_trim_chars_temp_array_leak_7522, borrowed_local_array_not_freed_spec_28_9) {
    JitResult r = runAotChecked(prog(
        "    var loc: Array<char> := [' ', 'a'];\n"
        "    while i < 200 do\n"
        "      n := n + \" xa \".Trim(loc).Length;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    n := n + loc.Count;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "202");
}

// §28.3 -- an `owned` array bound to a local is the local's to release; the
// call must neither leak nor free it.
BEZEL_TEST(string_trim_chars_temp_array_leak_7522, bound_owned_array_not_freed_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "    while i < 200 do\n"
        "      using var t := MkA();\n"
        "      n := n + \" xa \".Trim(t).Length + t.Count;\n"
        "      i := i + 1;\n"
        "    end while;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "600");
}

// CONTROL (#7222 funnel): a call-result `string` set stays clean.
BEZEL_TEST(string_trim_chars_temp_array_leak_7522, call_result_string_no_leak_control_spec_28_9) {
    JitResult r = runAotChecked(prog(
        "    while i < 200 do\n"
        "      n := n + \" xa \".Trim(MkS()).Length;\n"
        "      i := i + 1;\n"
        "    end while;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "200");
}
