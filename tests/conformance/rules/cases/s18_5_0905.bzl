//! case: s18_5_0905
//! doc: spec
//! section: 18.5
//! title: a ToString that throws inside an interpolation hole in an iterator propagates to the consuming
//!   for
//! rule: A hole *without* a format spec formats via the value's ordinary `ToString()`
//! cites: 18.5, 8.5
//! expect: run
//! output: got=30 caught=30
module Conf;

public class T
  public constructor()
  end constructor;

  public override method ToString(): string
    throw new InvalidOperationException("render");
  end ToString;
end T;

public class Program
  public static method Gen(): IEnumerable<string>
    using var t := new T();
    yield return "a";
    yield return $"{t}";
    yield return "z";
  end Gen;

  public static method Main()
    var caught := 0;
    var got := 0;
    var i := 0;
    while i < 30 do
      try
        for var s in Gen() do
          got := got + 1;
        end for;
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("got=" + got.ToString() + " caught=" + caught.ToString());
  end Main;
end Program;
