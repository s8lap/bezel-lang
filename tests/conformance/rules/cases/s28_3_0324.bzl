//! case: s28_3_0324
//! doc: spec
//! section: 28.3
//! title: a conditional move of an owning-choice local through ! disposes the payload once
//! rule: owned ⊓ moved-from and owned ⊓ explicitly-disposed = **maybe-consumed** — unreadable after
//!   the join, no flow around a back edge into a use; scope-end disposal per-path;
//! cites: K7, 15.3, 17.8
//! expect: run
//! output: 100
module Conf;

public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;

public choice OC
  Text(owned r: Res),
  Empty;
end OC;

public class Program
  public static method T(i: int)
    var o: OC? := OC.Text(new Res());
    if i mod 2 = 0 then
      var x := o!;
    end;
  end T;

  public static method Main(): int
    for var i in 0..<100 do
      T(i);
    end;
    Console.WriteLine($"{Res.Count}");
    return 0;
  end Main;
end Program;
