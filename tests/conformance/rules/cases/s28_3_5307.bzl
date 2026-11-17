//! case: s28_3_5307
//! doc: spec
//! section: 28.3
//! title: an async owning-choice local in a loop body is disposed once per iteration whichever path moved it
//! rule: owned ⊓ moved-from and owned ⊓ explicitly-disposed = **maybe-consumed** — unreadable after the
//!   join, no flow around a back edge into a use; scope-end disposal per-path;
//! cites: 28.3.1, 30.1
//! expect: run
//! output: 600 600 200
module Conf;

public class Res
  public static Made: int := 0;
  public static Count: int := 0;
  public N: int := 7;

  public constructor()
    Res.Made := Res.Made + 1;
  end constructor;

  destructor
    Res.Count := Res.Count + 1;
  end destructor;
end Res;

public choice OC Text(owned r: Res), Empty; end OC;

public class Program
  public static async method T(i: int): Task<int>
    var x: OC := OC.Empty;
    for var j in 0..<3 do
      var o := OC.Text(new Res());
      if ((i + j) mod 2) = 0 then x := o; end if;
      await Task.Delay(0);
    end;
    return 1;
  end T;
  public static async method Main(): Task
    var s := 0;
    for var i in 0..<200 do
      s := s + await T(i);
    end;
    Console.WriteLine($"{Res.Made} {Res.Count} {s}");
  end Main;
end Program;
