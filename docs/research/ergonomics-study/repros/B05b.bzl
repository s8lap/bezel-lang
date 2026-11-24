// bug: B5
// title: `Array<DictionaryEntry<K, V>>` is broken
// issue: #6557 (the Add leg)
// rests-on: stdlib §4.8: Dictionary iteration is in insertion order; §4.4 `Sort(comparer)`; `Reverse()`
// source: data/compiler-bugs.md (second repro block), verbatim below this header
// expect: run
// stdout: a4 z2 b1
// stdout: 3
// stdout: z2 a4 b1
module P;
public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    d["b"] := 1; d["a"] := 4; d["z"] := 2;
    var es := new Array<DictionaryEntry<string, int>>();
    for var e in d do es.Add(e); end;
    es.Sort(lambda(x, y) => y.Value - x.Value);
    for var e in es do Console.Write($"{e.Key}{e.Value} "); end;
    Console.WriteLine();
    var es2 := d.ToArray();
    Console.WriteLine(es2.Count);
    es2.Reverse();
    for var e in es2 do Console.Write($"{e.Key}{e.Value} "); end;
    Console.WriteLine();
  end Main;
end Program;
