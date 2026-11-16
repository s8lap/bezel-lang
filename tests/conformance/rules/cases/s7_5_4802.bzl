//! case: s7_5_4802
//! doc: spec
//! section: 7.5
//! title: a null-conditional Dictionary and Set Add and Clear perform on non-null and skip on null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. ... The receiver is evaluated first; if it is null the access does not occur and nothing to its right in the chain is evaluated
//! cites: 7.5, stdlib 4
//! expect: run
//! output: 2 42
//!   0
//!   3
//!   0
//!   1
//! issue: 8501
module Conf;

public class Counter
  public static calls: int := 0;
  public static method Next(): int
    calls := calls + 1;
    return 41;
  end Next;
end Counter;

public class Program
  public static method Main()
    using var d: Dictionary<string, int>? := ["a": 1];
    d?.Add("b", Counter.Next());
    var sum := 0;
    for var e in d! do
      sum := sum + e.Value;
    end;
    Console.WriteLine($"{d!.Count} {sum}");
    d?.Clear();
    Console.WriteLine(d!.Count);
    using var s: Set<int>? := [1, 2];
    s?.Add(7);
    Console.WriteLine(s!.Count);
    s?.Clear();
    Console.WriteLine(s!.Count);
    using var dn: Dictionary<string, int>? := null;
    using var sn: Set<int>? := null;
    dn?.Add("c", Counter.Next());
    sn?.Add(Counter.Next());
    Console.WriteLine(Counter.calls);
  end Main;
end Program;
