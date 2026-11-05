//! case: s18_5_0901
//! doc: spec
//! section: 18.5
//! title: a ToString that throws in an interpolation hole in a finally block reaches the enclosing catch
//! rule: A hole *without* a format spec formats via the value's ordinary `ToString()`
//! cites: 18.5, 8.5
//! expect: run
//! output: fin=30 caught=30
module Conf;

public class T
  public constructor()
  end constructor;

  public override method ToString(): string
    throw new InvalidOperationException("render");
  end ToString;
end T;

public class Program
  public static method Main()
    var caught := 0;
    var fin := 0;
    var i := 0;
    while i < 30 do
      var t := new T();
      try
        try
          fin := fin + 1;
        finally
          Console.WriteLine($"{t}");
        end;
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("fin=" + fin.ToString() + " caught=" + caught.ToString());
  end Main;
end Program;
