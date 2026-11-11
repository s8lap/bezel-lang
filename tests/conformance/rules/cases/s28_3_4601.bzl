//! case: s28_3_4601
//! doc: spec
//! section: 28.3
//! title: a closure cannot transfer a captured collection-literal owned local
//! rule: Inside a closure, a captured owned binding
//! cites: 28.3.4, 15.4
//! expect: reject
//! diagnostic: captured owned
module Conf;

public class Program
  public static method Take(x: owned Array<int>): int
    using var hold := move x;
    return hold.Count;
  end Take;
  public static method Run(n: int): int
    var a: Array<int> := [n, 2];
    var f := lambda(): int => Program.Take(move a);
    return f();
  end Run;
  public static method Main()
    Console.WriteLine(Run(1));
  end Main;
end Program;
