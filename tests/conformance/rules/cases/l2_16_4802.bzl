//! case: l2_16_4802
//! doc: stdlib
//! section: 2.16
//! title: Weak.TryUpgrade yields a handle while the value lives and null once the last handle is disposed
//! rule: a new strong handle if the value
//! cites: 28.13
//! expect: run
//! output: True
//!   u1 gone
//!   RD
//!   True
module Conf;

public class Res
  public V: int := 0;

  public constructor(v: int)
    V := v;
  end constructor;

  destructor
    Console.WriteLine("RD");
  end destructor;
end Res;

public class Program
  public static method Main()
    var s := new Shared<Res>(new Res(1));
    using var w := s.Downgrade();
    do
      using var u1 := w.TryUpgrade();
      Console.WriteLine(u1 <> null);
    end;
    Console.WriteLine("u1 gone");
    s.Dispose();
    using var u2 := w.TryUpgrade();
    Console.WriteLine(u2 = null);
  end Main;
end Program;
