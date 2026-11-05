//! case: s14_1_1405
//! doc: spec
//! section: 14.1
//! title: in Box<Guid> the parameter Guid types Box's own members, not another class's Guid members
//! rule: Type parameters of declarations that do not nest share no scope
//! cites: 14.1, 2.7
//! expect: run
//! output: 5
//!   36
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
public class Box<Guid>
  private v: Guid;
  public constructor(x: Guid)
    v := x;
  end constructor;
  public method Get(): Guid
    return v;
  end Get;
  public method Go(h: Holder): int
    var w: Guid := v;
    return h.G.ToString().Length;
  end Go;
end Box;
public class Program
  public static method Main()
    using var h := new Holder();
    var b := new Box<int>(5);
    Console.WriteLine(b.Get());
    Console.WriteLine(b.Go(h));
  end Main;
end Program;
