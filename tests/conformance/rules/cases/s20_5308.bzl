//! case: s20_5308
//! doc: spec
//! section: 20
//! title: an if-expression of an owned nullable local and null moves the local only when selected
//! rule: in an owned-consuming position — a binding, an assignment, a plain or `owned` argument, a
//!   `return`, an owning slot — the **selected** branch's value moves
//! cites: 20, 35.9, 28.3.1
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
  public static method T(i: int): int
    var o: OC? := OC.Text(new Res());
    var x: OC? := (if (i mod 2) = 0 then o else null);
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
