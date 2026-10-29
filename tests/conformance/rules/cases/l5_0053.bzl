//! case: l5_0053
//! doc: stdlib
//! section: 5
//! title: A user extension named Count on a class without a Count member is called
//! rule: Only an instance member blocks an extension (spec 26.5 rule 1); Bag declares no Count, so the user extension Count(predicate) is selected.
//! cites: 26.5
//! expect: run
//! output: 5
module Conf;

public class Bag
  public method Add2(): int => 1;
end Bag;

extension Bag
  public method Count(p: (int) -> bool): int => 5;
end;

public class Program
  public static method Main()
    var b := new Bag();
    Console.WriteLine(b.Count(lambda(x) => x > 1));
  end Main;
end Program;
