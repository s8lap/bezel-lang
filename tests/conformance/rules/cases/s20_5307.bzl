//! case: s20_5307
//! doc: spec
//! section: 20
//! title: an if-expression of an owned parameter and a fresh choice moves the parameter only if selected
//! rule: in an owned-consuming position — a binding, an assignment, a plain or `owned` argument, a
//!   `return`, an owning slot — the **selected** branch's value moves
//! cites: 20, 28.3.1, 28.3.4
//! expect: run
//! output: 300 300 200
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
  public static method Pick(owned p: OC, c: bool): int
    using var x := (if c then p else OC.Text(new Res()));
    return 1;
  end Pick;

  public static method T(i: int): int
    return Program.Pick(OC.Text(new Res()), (i mod 2) = 0);
  end T;
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      s := s + T(i);
    end;
    Console.WriteLine($"{Res.Made} {Res.Count} {s}");
  end Main;
end Program;
