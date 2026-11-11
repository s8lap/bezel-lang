//! case: s28_7_4604
//! doc: spec
//! section: 28.7
//! title: a borrow re-taken after the write is a fresh borrow
//! rule: not assigned between the borrow and the use
//! cites: 28.3, 7.4
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
    h.B := new Box();
    b := h.B;
    Console.WriteLine(b.V);
  end Main;
end Program;
