//! case: s31_1_1101
//! doc: spec
//! section: 31.1
//! title: an exception thrown inside a string extension is caught by the caller's arm for its class
//! rule: a thrown exception is caught by the first arm naming its class or a base of it
//! cites: 26.7
//! expect: run
//! output: 100
//! issue: 7841
module Conf;

public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;

public extension string
  public method Boom(): int
    if this.Length > 0 then
      throw new InvalidOperationException("b");
    end if;
    return 1;
  end Boom;
end;

public class Program
  public static method Main()
    var t := 0;
    for var i in 0 .. 99 do
      try
        t := t + Make.Str(i).Boom();
      catch
        InvalidOperationException => t := t + 1;
      end try;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
