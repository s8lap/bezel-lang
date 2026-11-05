//! case: s14_1_1414
//! doc: spec
//! section: 14.1
//! title: ToString of another class's Guid member, direct and through Object, inside Box<Guid>
//! rule: Type parameters of declarations that do not nest share no scope
//! cites: 14.1, 2.7
//! expect: run
//! output: 36036
//!   36036
module Conf;
public class Holder
  public property X: Guid;
  public constructor()
    X := Guid.NewGuid();
  end constructor;
end Holder;
public class Box<Guid>
  public method Go(h: Holder): int
    var s := h.X.ToString();
    var o: Object := h.X;
    var t := o.ToString();
    return s.Length * 1000 + t.Length;
  end Go;
end Box;
public class Program
  public static method Main()
    using var h := new Holder();
    var b := new Box<int>();
    Console.WriteLine(b.Go(h));
    var c := new Box<string>();
    Console.WriteLine(c.Go(h));
  end Main;
end Program;
