//! case: s14_1_1415
//! doc: spec
//! section: 14.1
//! title: ToString of another class's TimeSpan member, direct and through Object, inside Box<TimeSpan>
//! rule: Type parameters of declarations that do not nest share no scope
//! cites: 14.1, 2.7
//! expect: run
//! output: 16016
//!   16016
module Conf;
public class Holder
  public property X: TimeSpan;
  public constructor()
    X := TimeSpan.FromTicks(5L);
  end constructor;
end Holder;
public class Box<TimeSpan>
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
