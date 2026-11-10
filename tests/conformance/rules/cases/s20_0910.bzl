//! case: s20_0910
//! doc: spec
//! section: 20
//! title: marked transfers on both arms consume only the selected local; the other is disposed at scope end
//! rule: transfers, consuming on the path where its branch is selected and leaving the binding per-path consumed after the construct
//! cites: 28.3
//! expect: run
//! output: 15 30
module Conf;
public class W
  public static Dropped: int := 0;
  public constructor() end constructor;
  destructor W.Dropped := W.Dropped + 1; end destructor;
end W;
public class Program
  public static method Main()
    var t := 0;
    for var i in 0..<10 do
      var a := new OwningArray<W>();
      a.Add(new W());
      var b := new OwningArray<W>();
      b.Add(new W());
      b.Add(new W());
      using var x := (if i mod 2 = 0 then move a else move b);
      t += x.Count;
    end for;
    Console.WriteLine($"{t} {W.Dropped}");
  end Main;
end Program;
