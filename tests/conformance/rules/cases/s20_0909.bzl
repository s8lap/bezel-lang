//! case: s20_0909
//! doc: spec
//! section: 20
//! title: nested marked arm transfers consume only the selected local; the others are disposed per path
//! rule: transfers, consuming on the path where its branch is selected and leaving the binding per-path consumed after the construct
//! cites: 28.3
//! expect: run
//! output: 2 12
module Conf;
public class W
  public static Dropped: int := 0;
  public constructor() end constructor;
  destructor W.Dropped := W.Dropped + 1; end destructor;
end W;
public class Program
  static method Mk(c: bool, d: bool): owned OwningArray<W>
    var a := new OwningArray<W>();
    a.Add(new W());
    var b := new OwningArray<W>();
    return (if c then (if d then move a else move b) else new OwningArray<W>());
  end Mk;

  public static method Main()
    var t := 0;
    for var i in 0..<12 do
      t += Mk(i mod 2 = 0, i mod 3 = 0).Count;
    end for;
    Console.WriteLine($"{t} {W.Dropped}");
  end Main;
end Program;
