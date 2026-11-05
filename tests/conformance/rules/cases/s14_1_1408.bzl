//! case: s14_1_1408
//! doc: spec
//! section: 14.1
//! title: an interpolation hole and an Object conversion of a Guid member keep the Guid type inside Box<Guid>
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
public class Box<Guid>
  public method Go(h: Holder): int
    var s := $"{h.G}";
    var o: Object := h.G;
    return s.Length + o.ToString().Length;
  end Go;
end Box;
public class Program
  public static method Main()
    using var h := new Holder();
    var b := new Box<int>();
    Console.WriteLine(b.Go(h));
  end Main;
end Program;
