//! case: s28_7_4602
//! doc: spec
//! section: 28.7
//! title: a write on only one branch does not make the later use of the borrow an error
//! rule: A write on only some paths to the use, a window with an invalidating operation in it
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
    var n := "x".Length;
    if n > 5 then
      h.B := new Box();
    end;
    Console.WriteLine(b.V);
  end Main;
end Program;
