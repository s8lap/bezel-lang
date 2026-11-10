//! case: s20_0905
//! doc: spec
//! section: 20
//! title: marked arm transfer of an OwningArray local disposes each element exactly once on both paths
//! rule: transfers, consuming on the path where its branch is selected and leaving the binding per-path consumed after the construct
//! cites: 28.3, 28.11
//! expect: run
//! output: 10 20
module Conf;
public class W
  public static Dropped: int := 0;
  public constructor() end constructor;
  destructor W.Dropped := W.Dropped + 1; end destructor;
end W;
public class Program
  static method Mk(c: bool): owned OwningArray<W>
    var a := new OwningArray<W>();
    a.Add(new W());
    a.Add(new W());
    return (if c then move a else new OwningArray<W>());
  end Mk;

  public static method Main()
    var t := 0;
    for var i in 0..<10 do
      t += Mk(i mod 2 = 0).Count;
    end for;
    Console.WriteLine($"{t} {W.Dropped}");
  end Main;
end Program;
