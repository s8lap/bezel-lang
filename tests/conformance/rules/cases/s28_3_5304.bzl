//! case: s28_3_5304
//! doc: spec
//! section: 28.3
//! title: an async owning-choice local moved, reassigned and moved again across paths is disposed once per Res
//! rule: owned ⊓ moved-from and owned ⊓ explicitly-disposed = **maybe-consumed** — unreadable after the
//!   join, no flow around a back edge into a use; scope-end disposal per-path;
//! cites: 28.3.1, 28.7, 30.1
//! expect: run
//! output: 267 267 200
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
    var c := (i mod 3) = 0;
    var d := (i mod 2) = 0;
    var o := OC.Text(new Res());
    var x: OC := OC.Empty;
    var y: OC := OC.Empty;
    if c then
      x := o;
      await Task.Delay(0);
      o := OC.Text(new Res());
    end if;
    if d then y := o; end if;
    await Task.Delay(0);
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
