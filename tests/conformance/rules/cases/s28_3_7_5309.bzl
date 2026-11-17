//! case: s28_3_7_5309
//! doc: spec
//! section: 28.3.7
//! title: an owned choice local declared inside a closure may be moved inside it
//! rule: Inside a closure, a captured owned binding — an owned local or `owned` parameter of the
//!   enclosing body — is uniformly in **borrow state**: reads borrow (the producers rows above)
//! cites: 28.3.7, 28.9, 15.3
//! expect: run
//! output: 200 200 1400
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
    var f := lambda(): int => do
      var p := OC.Text(new Res());
      var q := p;
      return (case q of Text(r) => r.N, _ => 0);
    end;
    return f();
  end T;
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      s := s + T(i);
    end;
    Console.WriteLine($"{Res.Made} {Res.Count} {s}");
  end Main;
end Program;
