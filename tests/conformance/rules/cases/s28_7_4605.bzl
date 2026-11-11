//! case: s28_7_4605
//! doc: spec
//! section: 28.7
//! title: a use of a field borrow after a dominating overwrite of the field is rejected
//! rule: A use of the local — a read, a member access, an argument, a re-borrow — that such a write **dominates** on every path
//! cites: 28.3, 28.2
//! expect: reject
//! diagnostic: write
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
    Console.WriteLine(b.V);
  end Main;
end Program;
