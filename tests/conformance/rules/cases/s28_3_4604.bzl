//! case: s28_3_4604
//! doc: spec
//! section: 28.3
//! title: owned temporaries behind bang and ?. receivers are disposed when the result is stored or adopted
//! rule: Unbound owned temporaries are disposed at their full expression's end, after their uses, in reverse production order
//! cites: 17.8, 7.5
//! expect: run
//! stdout: live=0 closes=1600
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
      var n := Program.OpenOrNull()?.Id();
      var m := Program.OpenOrNull()!.Id();
      var p := Program.OpenOrNull()?.P;
      var q := Program.OpenOrNull()!.P;
      using var r := Program.OpenOrNull()?.Next();
      using var t := Program.OpenOrNull()!.NextNN();
      i := i + 1;
    end while;
    Console.WriteLine($"live={Conn.live} closes={Conn.closes}");
  end Main;
end Program;
