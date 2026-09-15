//! case: s28_3_0048
//! doc: spec
//! section: 28.3
//! title: an owned local live across an exception edge is disposed during unwinding
//! rule: Edge parity: owned locals live when an exception propagates out of their
//!   enclosing block are disposed implicitly during stack unwinding, exactly as on
//!   the normal edge.
//! cites: C.3
//! expect: run
//! output: disposed
//!   caught
module Conf;

public class Widget
  public constructor()
  end constructor;

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
