//! case: s14_1_1417
//! doc: spec
//! section: 14.1
//! title: control: the same shape with an ordinary type parameter T reads another class's Guid member
//! rule: Type parameters of declarations that do not nest share no scope
//! cites: 14.1, 2.7
//! expect: run
//! stdout: 136
module Conf;
public class Holder
  public property G: Guid;
  public constructor()
    G := Guid.NewGuid();
  end constructor;
end Holder;
public class Box<T>
  private v: T;
  public constructor(x: T)
    v := x;
  end constructor;
  public method Go(h: Holder): int
    var t: T := v;
    return h.G.ToString().Length + (case t of 5 => 100, _ => 0);
  end Go;
end Box;
public class Program
  public static method Main()
    using var h := new Holder();
    var b := new Box<int>(5);
    Console.WriteLine(b.Go(h));
  end Main;
end Program;
