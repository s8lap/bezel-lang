//! case: s27_6_0020
//! doc: spec
//! section: 27.6
//! title: a generic Math.Max reference with its list, passed as an argument, is formed at the parameter's type
//! rule: A method reference over a generic method names its type arguments on the member name; the reference is formed against the target slot it is passed to.
//! cites: 27.6
//! expect: run
//! stdout: 8
//! issue: 5814
module Conf;

public class Program
  public static method Apply(f: (int, int) -> int, a: int, b: int): int => f(a, b);

  public static method Main()
    Console.WriteLine(Apply(Math.Max<int>, 4, 8));
  end Main;
end Program;
