//! case: s29_11_4501
//! doc: spec
//! section: 29.11
//! title: a second enumeration of a lazy query after Count has spent its right is a
//!   compile error
//! rule: A second enumeration, or any other use after the first, is a compile error
//!   where the flow analysis can see it statically
//! cites: 28.3.10
//! expect: reject
//! diagnostic: enumerat
//! issue: 8392
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [5, 12, 30];
    using var big := xs.Where(lambda(x) => x > 10);
    Console.WriteLine(big.Count());
    for var x in big do
      Console.WriteLine(x);
    end;
  end Main;
end Program;
