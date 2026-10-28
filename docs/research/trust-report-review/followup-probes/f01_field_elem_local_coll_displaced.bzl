module Conf;
// Critic counterexample to edit r0131-spec-28.11 as drafted: a field-sourced element in a purely local collection, the field's value then taken by the body's own slot kill by move and transferred. Spec: the store is reported today (§28.11 report sentence); under the drafted carve-out nothing is.
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
  public static method W(owned q: Q?)
  end W;
  public method Run(): int
    using var tmp := new Array<Q>();
    tmp.Add(_f!);
    var c := move _f;
    using var t := Domain.Spawn<Q?>(W, move c);
    return tmp[0].M();
  end Run;
end R;
public class Program
  public static method Main(): int
    using var r := new R();
    return r.Run();
  end Main;
end Program;
