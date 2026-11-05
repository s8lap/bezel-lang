//! case: s14_1_1404
//! doc: spec
//! section: 14.1
//! title: a generic method whose type parameter is named Guid does not capture the Guid of another class
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
public class Box<K>
  public method Go(h: Holder): int
    return Inner<int>(h, 5) + Inner<string>(h, "ab");
  end Go;
  public method Inner<Guid>(h: Holder, x: Guid): int
    var y: Guid := x;
    return h.G.ToString().Length;
  end Inner;
end Box;
public class Program
  public static method Main()
    using var h := new Holder();
    var b := new Box<double>();
    Console.WriteLine(b.Go(h));
  end Main;
end Program;
