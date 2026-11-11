//! case: s28_3_4605
//! doc: spec
//! section: 28.3
//! title: an owned temporary is disposed on the exception edge when its member call throws
//! rule: Unbound owned temporaries are disposed at their full expression's end, after their uses, in reverse production order
//! cites: 17.8, 7.5
//! expect: run
//! stdout: live=0 closes=400
//! issue: 8398
module Conf;

public class Conn
  public static live: int := 0;
  public static closes: int := 0;
  public constructor()
    Conn.live := Conn.live + 1;
  end constructor;
  destructor
    Conn.live := Conn.live - 1;
    Conn.closes := Conn.closes + 1;
  end destructor;
  public method Id(): int => 7;
  public property P: int => 5;
  public method Next(): owned Conn? => new Conn();
  public method NextNN(): owned Conn => new Conn();
  public method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;
end Conn;

public class Holder
  private owned _c: Conn? := new Conn();
  public method Get(): Conn? => _c;
end Holder;

public class Program
  public static method OpenOrNull(): owned Conn? => new Conn();
  public static method NullConn(): owned Conn? => null;

  public static method Main()
    var i := 0;
    while i < 200 do
      try
        var z := Program.OpenOrNull()?.Boom();
      catch
        InvalidOperationException => Console.Write(""),
      end try;
      try
        var z2 := Program.OpenOrNull()!.Boom();
      catch
        InvalidOperationException => Console.Write(""),
      end try;
      var w := Program.NullConn()?.Id();
      i := i + 1;
    end while;
    Console.WriteLine($"live={Conn.live} closes={Conn.closes}");
  end Main;
end Program;
