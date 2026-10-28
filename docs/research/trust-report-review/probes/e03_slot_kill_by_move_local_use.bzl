module Conf;
// Variant of e02 inside ONE body (the shape 0206's window analysis sees): borrow of the slot's value, the replacing move of the same field, transfer of the taken value, use of the stale local borrow. Measure r35 (verdict, report, run).
public sealed class Q
  private _n: int := 0;
  public constructor()
  end constructor;
  public method M(): int => _n;
end Q;
public class R
  private owned _f: Q?;
  public constructor()
    _f := new Q();
  end constructor;
  public static method W(owned q: Q)
  end W;
  public method Run(): int
    var b := _f!;
    var c := move _f!;
    using var t := Domain.Spawn<Q>(W, move c);
    return b.M();
  end Run;
end R;
public class Program
  public static method Main(): int
    using var r := new R();
    return r.Run();
  end Main;
end Program;
