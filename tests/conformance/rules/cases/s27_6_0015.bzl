//! case: s27_6_0015
//! doc: spec
//! section: 27.6
//! title: a generic method reference passed as an argument without its list is rejected
//! rule: When a lambda's body is just a call to an existing method, the method reference may be used directly.
//!   A reference to a generic method with no list is a compile error naming the type parameters to supply, and no inference binds them from the target.
//! cites: 27.6, 14.5
//! expect: reject
//! diagnostic: type parameters
//! issue: 5814
module Conf;

public class Program
  public static method Apply(f: (int, int) -> int, a: int, b: int): int => f(a, b);

  public static method Main()
    Console.WriteLine(Apply(Math.Max, 4, 8));
  end Main;
end Program;
