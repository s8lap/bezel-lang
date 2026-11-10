//! case: s20_0907
//! doc: spec
//! section: 20
//! title: a throw before a marked arm transfer disposes the still-owned local on the unwinding path
//! rule: transfers, consuming on the path where its branch is selected and leaving the binding per-path consumed after the construct
//! cites: 28.3
//! expect: run
//! output: 4004 12
module Conf;
public class W
  public static Dropped: int := 0;
  public constructor() end constructor;
  destructor W.Dropped := W.Dropped + 1; end destructor;
end W;
public class Program
  static method Mk(c: bool, th: bool): owned OwningArray<W>
    var a := new OwningArray<W>();
    a.Add(new W());
    if th then
      throw new Exception("x");
    end if;
    return (if c then move a else new OwningArray<W>());
  end Mk;

  public static method Main()
    var t := 0;
    for var i in 0..<12 do
      try t += Mk(i mod 2 = 0, i mod 3 = 0).Count; catch var e: Exception => t += 1000; end try;
    end for;
    Console.WriteLine($"{t} {W.Dropped}");
  end Main;
end Program;
