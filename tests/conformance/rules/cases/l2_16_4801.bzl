//! case: l2_16_4801
//! doc: stdlib
//! section: 2.16
//! title: a Shared handle's Dispose drops the count and only the last disposal disposes the value
//! rule: Dispose(): count - 1; the last disposal disposes the underlying value.
//! cites: 28.13
//! expect: run
//! output: after first
//!   7
//!   RD
//!   after last
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
    var s := new Shared<Res>(new Res(7));
    var c := s.Clone();
    s.Dispose();
    Console.WriteLine("after first");
    Console.WriteLine(c.Get().V);
    c.Dispose();
    Console.WriteLine("after last");
  end Main;
end Program;
