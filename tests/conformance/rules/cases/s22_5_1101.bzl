//! case: s22_5_1101
//! doc: spec
//! section: 22.5
//! title: an argument expression that throws stops the string-extension call from running
//! rule: A call's **supplied** argument expressions are evaluated in **call-site textual order**
//! cites: 31.1
//! expect: run
//! output: caught
//!   caught
//!   caught
//! issue: 7841
module Conf;

public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;

public class Thr
  public static method Go(n: int): int
    if n >= 0 then
      throw new InvalidOperationException("a");
    end if;
    return 0;
  end Go;
end Thr;

public extension string
  public method Add2(k: int): int
    Console.WriteLine("ran");
    return this.Length + k;
  end Add2;
end;

public class Program
  public static method Main()
    var t := 0;
    for var i in 0 .. 2 do
      try
        t := t + Make.Str(i).Add2(Thr.Go(i));
      catch
        InvalidOperationException => Console.WriteLine("caught")
      end try;
    end for;
  end Main;
end Program;
