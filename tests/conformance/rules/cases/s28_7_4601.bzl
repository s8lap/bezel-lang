//! case: s28_7_4601
//! doc: spec
//! section: 28.7
//! title: a use of a field borrow before the field is overwritten is admitted
//! rule: Use the borrow before the write, or take the value out with the replacing `move`
//! cites: 28.3
//! expect: run
//! stdout: 3
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
    b.V := 3;
    var t := b.V;
    h.B := new Box();
    Console.WriteLine(t);
  end Main;
end Program;
