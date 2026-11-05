//! case: s14_1_1410
//! doc: spec
//! section: 14.1
//! title: an iterator method of Box<Guid> reading another class's Guid member keeps the Guid type
//! rule: Type parameters of declarations that do not nest share no scope
//! cites: 14.1, 2.7
//! expect: run
//! stdout: 114
module Conf;
public class Holder
  public property G: Guid;
  public constructor()
    G := Guid.NewGuid();
  end constructor;
end Holder;
public class Box<Guid>
  public method Seq(h: Holder, n: int): IEnumerable<int>
    for var i in 1..n do
      yield return h.G.ToString().Length + i;
    end;
  end Seq;
end Box;
public class Program
  public static method Main()
    using var h := new Holder();
    var b := new Box<int>();
    var total := 0;
    for var v in b.Seq(h, 3) do
      total := total + v;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
