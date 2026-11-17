//! case: s17_8_5301
//! doc: spec
//! section: 17.8
//! title: an async if-expression arm `o!` over an owning-choice local moves it exactly as the bare read does
//! rule: Over a named owned binding, `x!` is therefore classified by its position exactly as the bare
//!   read `x` is
//! cites: 17.8, 20, 28.3.4, 30.1
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
  public static async method T(i: int): Task<int>
    var c := (i mod 2) = 0;
    var o := OC.Text(new Res());
    await Task.Delay(0);
    var x: OC := (if c then o! else OC.Empty);
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
