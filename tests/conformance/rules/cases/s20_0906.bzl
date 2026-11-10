//! case: s20_0906
//! doc: spec
//! section: 20
//! title: a marked arm transfer after an early return leaves the local owned and disposed on the early path
//! rule: transfers, consuming on the path where its branch is selected and leaving the binding per-path consumed after the construct
//! cites: 28.3
//! expect: run
//! output: 4 12
module Conf;
public class W
  public static Dropped: int := 0;
  public constructor() end constructor;
  destructor W.Dropped := W.Dropped + 1; end destructor;
end W;
public class Program
  static method Mk(c: bool, early: bool): owned OwningArray<W>
    var a := new OwningArray<W>();
    a.Add(new W());
    if early then
      return new OwningArray<W>();
    end if;
    return (if c then move a else new OwningArray<W>());
  end Mk;

  public static method Main()
    var t := 0;
    for var i in 0..<12 do
      t += Mk(i mod 2 = 0, i mod 3 = 0).Count;
    end for;
    Console.WriteLine($"{t} {W.Dropped}");
  end Main;
end Program;
