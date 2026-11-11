//! case: l5_4701
//! doc: stdlib
//! section: 5
//! title: an async method returning a lazy query that is never enumerated runs no predicate
//! rule: All are lazy unless documented otherwise.
//! cites: 5.1, 30.1
//! expect: run
//! stdout: 200 0
module Conf;

public class Tr
  public static Pred: int := 0;

  public static method Check(x: int): bool
    Tr.Pred := Tr.Pred + 1;
    return x > 1;
  end Check;
end Tr;

public class Program
  public static async method Evens(xs: Array<int>): Task<IEnumerable<int>>
    return xs.Where(lambda(x) => Tr.Check(x));
  end Evens;

  public static async method Main(): Task
    var shared: Array<int> := [1, 2, 3];
    var t := 0;
    for var i in 0..<200 do
      var it := await Program.Evens(shared);
      t := t + 1;
    end for;
    Console.WriteLine($"{t} {Tr.Pred}");
  end Main;
end Program;
