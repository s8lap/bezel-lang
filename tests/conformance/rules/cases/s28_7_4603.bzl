//! case: s28_7_4603
//! doc: spec
//! section: 28.7
//! title: a write to a different field does not invalidate a borrow of an owned field
//! rule: A **disposing write** of the same path
//! cites: 28.3
//! expect: run
//! stdout: 0
module Conf;

public class Box
  public V: int := 0;
  public constructor() end constructor;
end Box;

public class Holder
  public owned B: Box;
  public owned N: Box? := null;
  public constructor()
    B := new Box();
  end constructor;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    var b := h.B;
    h.N := new Box();
    Console.WriteLine(b.V);
  end Main;
end Program;
