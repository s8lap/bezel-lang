//! case: s22_5_4502
//! doc: spec
//! section: 22.5
//! title: int.Parse bound by name parses in the named radix; a bad radix raises
//!   ArgumentException
//! rule: Named arguments may appear in any order after positional arguments.
//! cites: 22.6
//! expect: run
//! output: 5
//!   argexc-named
//!   argexc-pos
//! issue: 7602
module Conf;

class C
  public static method Main(): int
    var r: int := 2;
    var s: string := "101";
    Console.WriteLine(int.Parse(radix := r, s := s));
    var bad: int := 3;
    try
      Console.WriteLine(int.Parse(radix := bad, s := "1"));
    catch
      ArgumentException => Console.WriteLine("argexc-named"),
      Exception => Console.WriteLine("other-named")
    end try;
    try
      Console.WriteLine(int.Parse("1", bad));
    catch
      ArgumentException => Console.WriteLine("argexc-pos"),
      Exception => Console.WriteLine("other-pos")
    end try;
    return 0;
  end Main;
end C;
