// Codegen -- spec section 7.5 / 7.7 (v0.359) over stdlib section 4.17:
// `d?.FirstKey` / `d?.LastKey` on a nullable `SortedDictionary<K, V>`, issue 8759
// (the sibling of 8749's `?.First` / `?.Last`).
//
// stdlib section 4.17 declares `public pure readonly property FirstKey: K`
// ("least key; InvalidOperationException if empty") and `LastKey: K` ("greatest
// key; InvalidOperationException if empty"). Spec section 7.5: "`x?.M` returns
// null if `x` is null; otherwise it performs the access. The result type is
// always nullable". THE DEFECT (MEASURED at the base f2589f706): the `?.` node
// was left untyped and lowered to nothing (#3079), so the program printed no line.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

void expectOut(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// The issue's repro: present dictionary yields least / greatest key, a null
// receiver yields null (`?? -1`); `?.Count` stays the unchanged control.
BEZEL_TEST(null_cond_sorted_dict_first_last_key_8759, int_keys_present_and_null) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method C(d: SortedDictionary<int, int>?)
    Console.WriteLine(d?.Count ?? -1);
    Console.WriteLine(d?.FirstKey ?? -1);
    Console.WriteLine(d?.LastKey ?? -1);
  end C;
  public static method Main()
    using var d := new SortedDictionary<int, int>();
    d.Add(9, 1);
    d.Add(2, 1);
    d.Add(5, 1);
    C(d);
    C(null);
  end Main;
end Program;
)BZL"),
              "3\n2\n9\n-1\n-1\n-1\n");
}

// String keys take the heap-string path of the `.` form.
BEZEL_TEST(null_cond_sorted_dict_first_last_key_8759, string_keys) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method C(d: SortedDictionary<string, int>?)
    Console.WriteLine(d?.FirstKey ?? "none");
    Console.WriteLine(d?.LastKey ?? "none");
  end C;
  public static method Main()
    using var d := new SortedDictionary<string, int>();
    d.Add("pear", 1);
    d.Add("apple", 2);
    d.Add("zed", 3);
    C(d);
    C(null);
  end Main;
end Program;
)BZL"),
              "apple\nzed\nnone\nnone\n");
}

// Empty NON-null receiver: the access is performed and raises
// InvalidOperationException exactly as the `.` form does (section 7.5; stdlib 4.17).
BEZEL_TEST(null_cond_sorted_dict_first_last_key_8759, empty_non_null_raises) {
    expectOut(runAotChecked(R"BZL(
module P;
public class Program
  public static method F(d: SortedDictionary<int, int>?): int
    try
      return d?.FirstKey ?? -1;
    catch InvalidOperationException =>
      return -99;
    end;
  end F;
  public static method Main()
    using var d := new SortedDictionary<int, int>();
    Console.WriteLine(F(d));
    Console.WriteLine(F(null));
  end Main;
end Program;
)BZL"),
              "-99\n-1\n");
}
