//! case: s31_2_6439
//! doc: spec
//! section: 31.2
//! title: a bare throw caught by a nested arm preserves the SupersededChain and
//!   leaves the borrow readable
//! rule: returns the same in-flight exception to the runtime and preserves the
//!   chain
//! cites: 31.2, 31.4
//! expect: run
//! stdout: 2600
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
    for var i in 0..<200 do
      try
        try throw new ExA("first"); finally throw new ExB("second"); end try;
      catch
        var e: ExB => do
          var ch := e.SupersededChain;
          try
            throw;
          catch
            var e2: ExB => do total := total + e2.SupersededChain.Count; end
          end try;
          total := total + ch.Count + ch[0].Message.Length + e.Message.Length;
        end
      end try;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
