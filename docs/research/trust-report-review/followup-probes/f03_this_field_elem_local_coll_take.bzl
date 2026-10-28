module Conf;
// Critic probe (f02 with the this.-spelled source): field-sourced element in a purely local collection; the field value taken by a same-class method and transferred to another domain; then a read through the element.
public sealed class Q
  private _n: int := 0;
  public constructor()
  end constructor;
  public method M(): int => _n;
end Q;
public class R
  private owned _f: Q? := new Q();
  public constructor()
  end constructor;
  public static method W(owned q: Q)
  end W;
  public method Take(): owned Q => move _f!;
  public method Run(): int
    using var tmp := new Array<Q>();
    tmp.Add(this._f!);
    var c := Take();
    using var t := Domain.Spawn<Q>(W, move c);
    return tmp[0].M();
  end Run;
end R;
public class Program
  public static method Main(): int
    using var r := new R();
    return r.Run();
  end Main;
end Program;
