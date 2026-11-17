//! case: s28_3_5313
//! doc: spec
//! section: 28.3
//! title: a ?? of an owned nullable call result and a fresh owning choice is accepted
//! rule: owned ⊓ owned = owned, obligation carried forward (for expression arms: exactly one arm's value
//!   exists at runtime, so exactly one obligation);
//! cites: 28.3.1, 28.3.4, 35.9
//! expect: run
//! output: 200 200 200
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
  public static method Mk(i: int): OC? => OC.Text(new Res());

  public static method T(i: int): int
    var x: OC := Program.Mk(i) ?? OC.Empty;
    return 1;
  end T;
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      s := s + T(i);
    end;
    Console.WriteLine($"{Res.Made} {Res.Count} {s}");
  end Main;
end Program;
