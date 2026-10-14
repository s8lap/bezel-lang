// fix #4160 (§26.5 "Specificity ordering" worked example, §26.1, v0.338): the
// value side of the generic-extension `this[0]` case. The no-announcement side
// is pinned by tests/lit/generic_extension_this_indexer_no_dropped_note_4160.bzl
// (runAot fills diagnostics only on a reject, so a gtest cannot see a Warning on
// a program that compiles).
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string out(const std::string& p) {
    auto r = runAotChecked(p);
    return r.ran ? r.stdoutText : std::string("<did not run>");
}
}  // namespace

// #4160 value side (the no-warning side is pinned by the lit row
// generic_extension_this_indexer_no_dropped_note_4160.bzl): the §26.5 worked
// example's output stays correct after the template-body announcement is
// suppressed — each specialization still lowers `this[0]` and reads the data.
BEZEL_TEST(codegen_generic_extension_this_indexer_4160, generic_extension_this_index_values_4160) {
    BEZEL_EXPECT_EQ(out(R"(module M;
extension Array<T>
  public method First(): T
    if this.Count = 0 then throw new InvalidOperationException("empty array"); end;
    return this[0];
  end First;
  public method SetFirst(v: T)
    this[0] := v;
  end SetFirst;
  public method Swap01()
    var t := this[0];
    this[0] := this[1];
    this[1] := t;
  end Swap01;
end;
extension Dictionary<K, V>
  public method Peek(k: K): V => this[k];
end;
public static class Program
  public static method Main(): int
    var xs: Array<int> := [7, 2, 3];
    var ys: Array<string> := ["a", "b"];
    xs.SetFirst(9);
    xs.Swap01();
    var d := new Dictionary<string, int>();
    d["a"] := 5;
    Console.WriteLine(xs.First().ToString() + ys.First() + d.Peek("a").ToString());
    return 0;
  end Main;
end;
)"), std::string("2a5\n"));
}
