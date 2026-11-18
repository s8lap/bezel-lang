//! case: s28_9_7583
//! doc: spec
//! section: 28.9
//! title: a string returned through a static method reference and a bound method reference stays valid
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9
//! expect: run
//! output: borrowed
//!   39
module Conf;
public class Box
  private _s: string := "";
  public method Put(v: string)
    _s := v;
  end Put;
  public method Get(): string => _s;
  public static method Hello(): string => "he" + "llo";
end Box;
public class Program

  public static method MkG(): () -> string => Box.Hello;
  public static method MkB(b: Box): () -> string => b.Get;
  public static method Run(): int
    var b := new Box();
    b.Put("bor" + "rowed");
    var t := 0;
    for var j in 0..<3 do
      var r := Program.MkG()();
      var q := Program.MkB(b)();
      t := t + r.Length + q.Length;
    end;
    Console.WriteLine(b.Get());
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
