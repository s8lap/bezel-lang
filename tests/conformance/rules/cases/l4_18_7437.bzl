//! case: l4_18_7437
//! doc: stdlib
//! section: 4.18
//! title: First and Last of an empty SortedSet<string> raise InvalidOperationException
//! rule: `First` and `Last` raise `InvalidOperationException` when the set is empty.
//! cites: 4.18, 9.6
//! expect: run
//! output: threw1
//!   threw2
module Conf;
public class Program
  public static method Main()
    using var s := new SortedSet<string>();
    try
      Console.WriteLine(s.First);
    catch InvalidOperationException => Console.WriteLine("threw1");
    end;
    try
      Console.WriteLine(s.Last);
    catch InvalidOperationException => Console.WriteLine("threw2");
    end;
  end Main;
end Program;
