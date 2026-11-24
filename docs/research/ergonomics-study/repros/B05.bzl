// bug: B5
// title: `Array<DictionaryEntry<K, V>>` is broken
// issue: #8385 (the sort leg)
// rests-on: stdlib §4.8 DictionaryEntry is a record; §5 `ToArray()`; §4.4 `Sort(comparer)`, not guaranteed stable (so the tied a4/c4 may swap in the second line)
// source: data/compiler-bugs.md (first repro block), verbatim below this header
// expect: run
// stdout: a4 c4 z2 b1
// stdout-re: (a4 c4|c4 a4) z2 b1
// stdout: b,m,q
module P;
public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    d["b"] := 1; d["a"] := 4; d["c"] := 4; d["z"] := 2;
    var es := d.ToArray();
    es.Sort(lambda(x, y) => (if x.Value <> y.Value then y.Value - x.Value else x.Key.CompareTo(y.Key)));
    for var e in es do Console.Write($"{e.Key}{e.Value} "); end;
    Console.WriteLine();
    es.Sort(lambda(x, y) => y.Value - x.Value);
    for var e in es do Console.Write($"{e.Key}{e.Value} "); end;
    Console.WriteLine();
    var ks: Array<string> := ["q", "b", "m"];
    ks.Sort(lambda(x, y) => x.CompareTo(y));
    Console.WriteLine(String.Join(",", ks));
  end Main;
end Program;
