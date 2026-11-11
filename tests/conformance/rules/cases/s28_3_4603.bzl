//! case: s28_3_4603
//! doc: spec
//! section: 28.3
//! title: an owned temporary behind a bang or ?. receiver in a return is disposed at the expression end
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
  public static method H(): int
    return Program.OpenOrNull()!.Id();
  end H;
  public static method H2(): int
    return Program.OpenOrNull()?.Id() ?? 0;
  end H2;
  public static method Main()
    var i := 0;
    while i < 200 do
      var s := Program.H() + Program.H2();
      i := i + 1;
    end while;
    Console.WriteLine($"live={Conn.live} closes={Conn.closes}");
  end Main;
end Program;
