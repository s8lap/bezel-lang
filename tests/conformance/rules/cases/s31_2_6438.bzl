//! case: s31_2_6438
//! doc: spec
//! section: 31.2
//! title: a user-thrown superseded exception is recorded in the superseding
//!   exception's SupersededChain
//! rule: an `ExceptionInfo` snapshot of the superseded exception itself — its type
//!   name, `Message`, and `StackTrace`
//! cites: 31.2
//! expect: run
//! stdout: 3000
module Conf;
public class ExA: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExA;
public class ExB: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExB;
public class ExX: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExX;
public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<500 do
      try
        try
          throw new ExA("first");
        finally
          throw new ExB("second");
        end try;
      catch
        var e: ExB => do
          var ch := e.SupersededChain;
          for var info in ch do total := total + info.Message.Length; end for;
          total := total + e.SupersededChain.Count;
        end
      end try;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
