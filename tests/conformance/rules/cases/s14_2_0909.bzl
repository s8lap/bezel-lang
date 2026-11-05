//! case: s14_2_0909
//! doc: spec
//! section: 14.2
//! title: Math.Max/Min<V> over user types conforming to IComparable directly or through an extending interface
//! rule: the constraint is decided at that site, by `P`'s own constraints. `P` satisfies the
//!   constraint exactly when its constraints guarantee it at every substitution they admit,
//!   as a matter of language rule ... a type-bound constraint naming I or an interface
//!   extending I, or a class bound naming a class that conforms to I under rule 1, directly
//!   or through a `where T: U` chain.
//! cites: 14.5, 27.6
//! expect: run
//! output: 4
//!   5
//!   9
//! issue: 6295
module Conf;

public record Pt(X: int, Y: int): IComparable<Pt>
  public method CompareTo(other: Pt): int
    return X - other.X;
  end CompareTo;
end Pt;

public class Cls: IComparable<Cls>
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;

  public method Nv(): int => _n;

  public method CompareTo(other: Cls): int => _n - other.Nv();
end Cls;

public interface J<T>: IComparable<T>
end J;

public record Q(V: int): J<Q>
  public method CompareTo(other: Q): int
    return V - other.V;
  end CompareTo;
end Q;

public class Helper
  public static method F<V>(): (V, V) -> V where V: IComparable<V>
    return Math.Max<V>;
  end F;

  public static method M<V>(): (V, V) -> V where V: IComparable<V>
    return Math.Min<V>;
  end M;

  public static method FJ<V>(): (V, V) -> V where V: J<V>
    return Math.Max<V>;
  end FJ;
end Helper;

public class Program
  public static method Main()
    var f: (Pt, Pt) -> Pt := Helper.F<Pt>();
    Console.WriteLine(f(new Pt(1, 2), new Pt(3, 4)).Y);
    using var c7 := new Cls(7);
    using var c5 := new Cls(5);
    var m: (Cls, Cls) -> Cls := Helper.M<Cls>();
    Console.WriteLine(m(c7, c5).Nv());
    var j: (Q, Q) -> Q := Helper.FJ<Q>();
    Console.WriteLine(j(new Q(1), new Q(9)).V);
  end Main;
end Program;
