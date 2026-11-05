//! case: s18_5_0903
//! doc: spec
//! section: 18.5
//! title: a ToString that throws inside an interpolation hole in a lambda body propagates to the caller's
//! rule: A hole *without* a format spec formats via the value's ordinary `ToString()`
//! cites: 18.5, 8.5
//! expect: run
//! output: caught=30
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
    var i := 0;
    while i < 30 do
      var t := new T();
      var f := lambda(): string => $"{t}";
      try
        var s := f();
        Console.WriteLine(s);
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("caught=" + caught.ToString());
  end Main;
end Program;
