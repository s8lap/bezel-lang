//! case: s20_5304
//! doc: spec
//! section: 20
//! title: a case scrutinee over a null-coalescing of a nullable field and a local borrows both operands
//! rule: in a borrowing position — an operand, a receiver, an index — every branch is a borrow of its
//!   binding.
//! cites: 20, 35.9, 28.3.1
//! expect: run
//! output: 600 600 1400
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

public class Holder
  public owned F: OC := OC.Text(new Res());
  public owned G: OC? := OC.Text(new Res());

  public constructor() end constructor;
end Holder;

public class Program
  public static method T(i: int): int
    using var h := new Holder();
    using var o := OC.Text(new Res());
    return (case (h.G ?? o) of Text(r) => r.N, _ => 0);
  end T;
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      s := s + T(i);
    end;
    Console.WriteLine($"{Res.Made} {Res.Count} {s}");
  end Main;
end Program;
