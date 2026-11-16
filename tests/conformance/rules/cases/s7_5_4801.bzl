//! case: s7_5_4801
//! doc: spec
//! section: 7.5
//! title: a null-conditional Array mutator performs on a non-null receiver; a null skips it and its argument
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access. ... The receiver is evaluated first; if it is null the access does not occur and nothing to its right in the chain is evaluated
//! cites: 7.5, stdlib 4
//! expect: run
//! output: [1,2,3,]
//!   [3,2,1,]
//!   [3,2,1,41,]
//!   [9,3,2,1,41,]
//!   [9,2,1,41,]
//!   []
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
  public static method Show(a: Array<int>): string
    var s := "";
    for var e in a do
      s := s + $"{e},";
    end;
    return $"[{s}]";
  end Show;

  public static method Main()
    using var x: Array<int>? := [3, 1, 2];
    x?.Sort();
    Console.WriteLine(Show(x!));
    x?.Reverse();
    Console.WriteLine(Show(x!));
    x?.Add(Counter.Next());
    Console.WriteLine(Show(x!));
    x?.Insert(0, 9);
    Console.WriteLine(Show(x!));
    x?.RemoveAt(1);
    Console.WriteLine(Show(x!));
    x?.Clear();
    Console.WriteLine(Show(x!));
    using var z: Array<int>? := null;
    z?.Add(Counter.Next());
    z?.Sort();
    Console.WriteLine(Counter.calls);
  end Main;
end Program;
