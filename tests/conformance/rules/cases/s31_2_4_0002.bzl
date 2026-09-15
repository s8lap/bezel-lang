//! case: s31_2_4_0002
//! doc: spec
//! section: 31.2.4
//! title: the bare re-raise statement re-throws after other statements in the arm
//! rule: As a statement: `throw;` (with the trailing semicolon), equivalent to
//!   the expression form followed by `;`, used inside `do ... end` blocks
//!   where the re-raise is one statement among several.
//! cites: 31.2.4
//! expect: run
//! stdout: logged
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message)
  end constructor;
end BoomException;

public class Program
  public static method Main()
    try
      try
        throw new BoomException("original");
      catch
        var ex: BoomException => do
          Console.WriteLine("logged");
          throw;
        end,
      end try;
    catch
      var outer: Exception => Console.WriteLine(outer.Message),
    end try;
  end Main;
end Program;
