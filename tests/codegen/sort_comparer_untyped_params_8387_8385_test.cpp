// Regression tests for #8387 and #8385 (stdlib §4.4 / §4.5, spec §14.5 and
// §27.4, v0.357): an un-annotated two-parameter comparer lambda passed to
// `Array<T>.Sort` takes its parameter types from the formal
// `comparer: (T, T) -> int` ([§27.4], "when the lambda's target type is known
// from context, parameter type annotations may be omitted"), exactly as the
// annotated spelling does.
//
// #8387: over `Array<W>` with a `string` member `K`,
//   `ss.Sort(lambda(x, y) => x.K.CompareTo(y.K))` left the array unsorted.
// #8385: over `d.ToArray()` (an `Array<DictionaryEntry<string, int>>`), any
//   un-annotated comparer left the array unsorted.
//
// The repro programs are the issues' own.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// #8387 — the typed spelling, an untyped `int`-member comparer, and the
// untyped `string`-member comparer on one record array. The `string` member
// comparer is the bug; the other two are the issue's controls.
BEZEL_TEST(sort_comparer_untyped_params_8387, string_member_compareto_sorts_spec_27_4) {
    const auto r = runAot(R"(module P;
record W(K: string, V: int);
public class Program
  public static method Main()
    var es: Array<W> := [new W("it", 4), new W("was", 4), new W("best", 1), new W("of", 4)];
    es.Sort(lambda(x: W, y: W): int => (if x.V <> y.V then y.V - x.V else x.K.CompareTo(y.K)));
    for var e in es do Console.Write($"{e.K}{e.V} "); end;
    Console.WriteLine();
    var ss: Array<W> := [new W("it", 4), new W("was", 4), new W("best", 1), new W("of", 4)];
    ss.Sort(lambda(x, y) => x.K.CompareTo(y.K));
    for var e in ss do Console.Write($"{e.K}{e.V} "); end;
    Console.WriteLine();
    var ts: Array<W> := [new W("it", 4), new W("was", 4), new W("best", 1), new W("of", 4)];
    ts.Sort(lambda(x, y) => y.V - x.V);
    Console.WriteLine(ts[3].K);
  end Main;
end Program;
)");
    BEZEL_EXPECT_EQ(r.ran ? r.stdoutText : std::string("<did-not-run>"), "it4 of4 was4 best1 \nbest1 it4 of4 was4 \nbest\n");
}

// #8385 — the issue's own repro: an untyped comparer over the
// `DictionaryEntry<string, int>` array `d.ToArray()`.
BEZEL_TEST(sort_comparer_untyped_params_8385, dictionary_entry_array_sorts_spec_27_4) {
    const auto r = runAot(R"(module P;
public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    d["b"] := 1; d["a"] := 4; d["c"] := 4; d["z"] := 2;
    var es := d.ToArray();
    es.Sort(lambda(x, y) => (if x.Value <> y.Value then y.Value - x.Value else x.Key.CompareTo(y.Key)));
    for var e in es do Console.Write($"{e.Key}{e.Value} "); end;
    Console.WriteLine();
    es.Sort(lambda(x, y) => y.Value - x.Value);
    Console.WriteLine(es[3].Key);
    var ks: Array<string> := ["q", "b", "m"];
    ks.Sort(lambda(x, y) => x.CompareTo(y));
    Console.WriteLine(String.Join(",", ks));
  end Main;
end Program;
)");
    BEZEL_EXPECT_EQ(r.ran ? r.stdoutText : std::string("<did-not-run>"), "a4 c4 z2 b1 \nb\nb,m,q\n");
}
