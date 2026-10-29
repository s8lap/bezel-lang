//! case: l5_0033
//! doc: stdlib
//! section: 5
//! title: a field and a method result typed as a list interface reach the section 5 operators
//! rule: The section 5 operators are extension methods on IEnumerable<T>; IList<T> and IReadOnlyList<T> extend it (stdlib 4.1), so they resolve on a field read, a call result and a property read alike.
//! cites: 4.1, 5
//! expect: run
//! output: 9
//!   3
//!   5
module Conf;

public class Holder
  private owned _a: Array<int>;
  private _l: IList<int>;

  public constructor()
    _a := [4, 5, 6];
    _l := _a;
  end constructor;

  public method Total(): int
    var t := _l.ToArray();
    return t.Count + t[2];
  end Total;

  public method Get(): IList<int> => _l;
  public method GetRo(): IReadOnlyList<int> => _a;
end Holder;

public class Program
  public static method Main()
    var h := new Holder();
    Console.WriteLine(h.Total());
    Console.WriteLine(h.Get().ToArray().Count);
    Console.WriteLine(h.GetRo().ToArray()[1]);
  end Main;
end Program;
