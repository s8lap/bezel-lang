//! case: s31_1_0008
//! doc: spec
//! section: 31.1
//! title: throwing a named owned local is written unmarked
//! rule: `throw` is a marker-free consuming position: the correct spelling of
//!   a named owned binding as a throw operand is `throw x;`, with no `move`.
//! cites: 31.1, 28.3
//! expect: run
//! stdout: boom
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message)
  end constructor;
end BoomException;

public class Program
  public static method Main()
    var x := new BoomException("boom");
    try
      throw x;
    catch
      var ex: BoomException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
