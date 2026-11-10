//! case: s20_0908
//! doc: spec
//! section: 20
//! title: a continue before a marked arm transfer disposes the still-owned local on that edge
//! rule: transfers, consuming on the path where its branch is selected and leaving the binding per-path consumed after the construct
//! cites: 28.3, 28.10
//! expect: run
//! output: 4 10
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
      if i mod 5 = 0 then
        continue;
      end if;
      using var x := (if i mod 2 = 0 then move a else new OwningArray<W>());
      t += x.Count;
    end for;
    Console.WriteLine($"{t} {W.Dropped}");
  end Main;
end Program;
