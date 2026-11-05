//! case: s18_5_0907
//! doc: spec
//! section: 18.5
//! title: interpolation holes over a borrowed parameter render in finally, a destructor and a catch arm
//! rule: A hole *without* a format spec formats via the value's ordinary `ToString()`
//! cites: 18.5, 8.5
//! expect: run
//! output: fin p
//!   p
//!   arm p boom
//!   fin p
//!   p
//!   arm p boom
//!   caught=2 tally=12
module Conf;

public class P
  public constructor()
  end constructor;

  public override method ToString(): string => "p";
end P;

public class D
  private owned _p: P := new P();

  public constructor()
  end constructor;

  destructor
    var o: Object := _p;
    Program.Tally := Program.Tally + $"dtor {o}".Length;
  end destructor;
end D;

public class Program
  public static Tally: int := 0;

  public static method Boom()
    throw new InvalidOperationException("boom");
  end Boom;

  public static method Inner(p: P)
    using var d := new D();
    try
      Boom();
    finally
      var o: Object := p;
      Console.WriteLine($"fin {o}");
      Console.WriteLine(p.ToString());
    end;
  end Inner;

  public static method Main()
    using var p := new P();
    var caught := 0;
    var i := 0;
    while i < 2 do
      try
        Inner(p);
      catch
        var ex: InvalidOperationException => do
          var o: Object := p;
          Console.WriteLine($"arm {o} {ex.Message}");
          caught := caught + 1;
        end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("caught=" + caught.ToString() + " tally=" + Program.Tally.ToString());
  end Main;
end Program;
