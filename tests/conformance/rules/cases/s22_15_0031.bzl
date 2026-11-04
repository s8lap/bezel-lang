//! case: s22_15_0031
//! doc: spec
//! section: 22.15
//! title: reading the components of stdlib positional records is admitted in a pure body
//! rule: an **auto-property getter** is pure (a backing-field read)
//! cites: 22.15, 9.1
//! expect: run
//! issue: 7453
//! output: 4
//!   7
module Conf;

public class Ops
  public static pure method Span(s: ReadOnlyStringSpan): int => s.Start + s.Length;
  public static pure method Entry(d: DictionaryEntry<string, int>): int => d.Value + d.Key.Length;
end Ops;

public class Program
  public static method Main()
    var s := new ReadOnlyStringSpan("hello", 1, 3);
    Console.WriteLine(Ops.Span(s));
    using var d := new Dictionary<string, int>();
    d.Add("ab", 5);
    for var e in d do
      Console.WriteLine(Ops.Entry(e));
    end for;
  end Main;
end Program;
