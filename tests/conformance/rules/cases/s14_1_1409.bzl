//! case: s14_1_1409
//! doc: spec
//! section: 14.1
//! title: passing another class's Guid member to a Guid parameter inside Box<Guid> keeps the Guid type
//! rule: Type parameters of declarations that do not nest share no scope
//! cites: 14.1, 2.7
//! expect: run
//! stdout: 72
module Conf;
public class Holder
  public property G: Guid;
  public property S: string;
  public owned A: Array<Guid>;
  public owned D: Dictionary<Guid, int>;
  public constructor()
    G := Guid.NewGuid();
    S := "hello";
    A := new Array<Guid>();
    A.Add(G);
    D := new Dictionary<Guid, int>();
    D[G] := 7;
  end constructor;
  public method Get(): Guid
    return G;
  end Get;
  public method MakeStr(): string
    var x := Guid.NewGuid();
    return x.ToString();
  end MakeStr;
end Holder;
public class Util
  public static method Len(g: Guid): int
    return g.ToString().Length;
  end Len;
end Util;
public class Inner<K>
  public method Show(h: Holder): int
    return Util.Len(h.G);
  end Show;
end Inner;
public class Box<Guid>
  public method Go(h: Holder): int
    var i := new Inner<Guid>();
    return i.Show(h) + Util.Len(h.G);
  end Go;
end Box;
public class Program
  public static method Main()
    using var h := new Holder();
    var b := new Box<int>();
    Console.WriteLine(b.Go(h));
  end Main;
end Program;
