//! case: s28_3_0048
//! doc: spec
//! section: 28.3
//! title: an owned local live across an exception edge is disposed during unwinding
//! rule: Edge parity: the exception edge performs the same implicit disposal as the
//!   normal edge, on the single establishment-ordered cleanup stack.
//! expect: run
//! stdout: disposed
module Conf;

public class Widget
  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    try
      var w := new Widget();
      throw new InvalidOperationException("boom");
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
