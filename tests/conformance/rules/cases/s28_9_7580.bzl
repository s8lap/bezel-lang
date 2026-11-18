//! case: s28_9_7580
//! doc: spec
//! section: 28.9
//! title: a string captured through two nested lambdas is read back unchanged on every iteration
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! output: hithere
//!   42
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

  public static method Run(): int
    var s := "hi" + "there";
    var f2 := lambda() => lambda() => s;
    var t := 0;
    for var j in 0..<3 do
      var r := f2()();
      t := t + r.Length + s.Length;
    end;
    Console.WriteLine(s);
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
