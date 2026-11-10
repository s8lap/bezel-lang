//! case: s19_2_0901
//! doc: spec
//! section: 19.2
//! title: an expression-case arm `move x` consumes only the selected local; the others are disposed per path
//! rule: the named binding is consumed on the path where that arm is selected, and is per-path consumed after the construct
//! cites: 20, 28.3
//! expect: run
//! output: 9 27
module Conf;
public class W
  public static Dropped: int := 0;
  public constructor() end constructor;
  destructor W.Dropped := W.Dropped + 1; end destructor;
end W;
public class Program
  static method Mk(k: int): owned OwningArray<W>
    var a := new OwningArray<W>();
    a.Add(new W());
    var b := new OwningArray<W>();
    b.Add(new W());
    b.Add(new W());
    return (case k of 0 => move a, 1 => move b, _ => new OwningArray<W>());
  end Mk;

  public static method Main()
    var t := 0;
    for var i in 0..<9 do
      t += Mk(i mod 3).Count;
    end for;
    Console.WriteLine($"{t} {W.Dropped}");
  end Main;
end Program;
