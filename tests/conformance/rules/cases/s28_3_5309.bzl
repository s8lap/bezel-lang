//! case: s28_3_5309
//! doc: spec
//! section: 28.3
//! title: an async using var owning-choice local is disposed at scope end after the suspension
//! rule: **owned** — the holder carries the disposal obligation: dispose exactly once, on every path.
//! cites: 28.3.1, 28.10, 30.1
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
    using var o := OC.Text(new Res());
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
