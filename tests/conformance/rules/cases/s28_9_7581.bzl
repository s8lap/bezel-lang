//! case: s28_9_7581
//! doc: spec
//! section: 28.9
//! title: a string captured by a nested lambda observes a later reassignment of the shared capture cell
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 27.5
//! expect: run
//! output: 5
//!   6
//!   24
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
    var f2 := lambda() => lambda() => s;
    var t := 0;
    var keep := "";
    for var j in 0..<3 do
      var r := f2()();
      s := s + "y";
      keep := r;
      t := t + r.Length + keep.Length;
    end;
    Console.WriteLine(keep.Length);
    Console.WriteLine(s.Length);
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
