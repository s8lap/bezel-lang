//! case: s22_6_2307
//! doc: spec
//! section: 22.6
//! title: a lambda returning a Dictionary at an
//!   IEnumerable<DictionaryEntry> slot iterates the entries
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 27.3
//! expect: run
//! output: 3
//! issue: 8289
//! notes: Red on base: the pre-fix compiler handed the raw collection
//!   pointer on as the IEnumerable and the caller's for-in crashed
//!   (segmentation fault), the staging tip converts and iterates.
module Conf;

public class H
  public static method Run(d: Dictionary<string, int>, f: (Dictionary<string, int>) -> IEnumerable<DictionaryEntry<string, int>>): int
    var n := 0;
    for var kv in f(d) do n := n + kv.Value; end;
    return n;
  end Run;
end H;

public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    d.Add("a", 1);
    d.Add("b", 2);
    Console.WriteLine(H.Run(d, lambda(l) => l));
  end Main;
end Program;
