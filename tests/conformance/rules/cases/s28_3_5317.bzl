//! case: s28_3_5317
//! doc: spec
//! section: 28.3
//! title: an if-expression of an owned choice field and a fresh owning choice is rejected
//! rule: owned ⊓ borrowed = **rejected** — no single state is correct, and demoting owned would silently
//!   drop an obligation
//! cites: 20, 28.3.1, 28.3.4
//! expect: reject
//! diagnostic: meet is rejected
module Conf;

public class Res
  public N: int := 7;

  public constructor() end constructor;

  destructor
    Console.WriteLine("released");
  end destructor;
end Res;

public choice OC Text(owned r: Res), Empty; end OC;

public class Holder
  public owned F: OC := OC.Text(new Res());

  public constructor() end constructor;
end Holder;

public class Program
  public static method T(i: int): int
    var c := (i mod 2) = 0;
    using var h := new Holder();
    var x: OC := (if c then h.F else OC.Text(new Res()));
    return 1;
  end T;
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      s := s + T(i);
    end;
    Console.WriteLine(s);
  end Main;
end Program;
