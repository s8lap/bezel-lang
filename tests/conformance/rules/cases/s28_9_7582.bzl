//! case: s28_9_7582
//! doc: spec
//! section: 28.9
//! title: a string produced by a nested lambda is stored into a field twice and read back
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9
//! expect: run
//! output: hix!
//!   36
module Conf;
public class Box
  private _s: string := "";
  public method Put(v: string)
    _s := v;
  end Put;
  public method Get(): string => _s;
end Box;
public interface IGreet
  method Greet(): string;
end IGreet;
public class G1 : IGreet
  private _n: string := "";
  public constructor(n: string)
    _n := n;
  end constructor;
  public method Greet(): string => _n;
end G1;
public class Program

  public static method Run(): int
    var s := "hi" + "x";
    var f2 := lambda() => lambda() => s + "!";
    var b := new Box();
    var t := 0;
    for var j in 0..<3 do
      var r := f2()();
      b.Put(r);
      var r2 := f2()();
      b.Put(r2);
      t := t + r.Length + r2.Length + b.Get().Length;
    end;
    Console.WriteLine(b.Get());
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
