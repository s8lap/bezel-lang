//! case: l5_0071
//! doc: stdlib
//! section: 5
//! title: Deferred operators over IList and IReadOnlyList receivers convert to IEnumerable
//! rule: public method Where(predicate: (T) -> bool): IEnumerable<T>;
//! cites: 4.1, 5, 5.1
//! expect: run
//! output: 3
//!   2
//!   12
//! issue: 7625
//! notes: twin of l5_0068 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Cnt(e: IEnumerable<int>): int
    return e.Count();
  end Cnt;

  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    using var q := l.Where(lambda(x: int): bool => x > 1);
    Console.WriteLine(Cnt(q));
    using var e: IEnumerable<int> := r.Take(2);
    Console.WriteLine(e.Count());
    var n := 0;
    for var v in r.Skip(1) do n := n + v; end;
    Console.WriteLine(n);
  end Main;
end Program;
