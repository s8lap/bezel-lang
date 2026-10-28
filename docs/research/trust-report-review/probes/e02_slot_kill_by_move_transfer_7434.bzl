module Conf;
// #7434 / bzcore F1: a local borrow of a slot's value, the replacing move (nullable-field bare form, §28.8.1 detach), a domain transfer, then a use. K4.3's slot-kill-by-move clause (§28.3 Scope clause 1) is "a recorded obligation" with no holder store to report at. EXPECT (spec): accepted; no report site today. Measure r35.
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
  public property F: Q => _f!;
  public method Take(): owned Q => move _f!;
end R;
public class Program
  public static method W(owned q: Q)
  end W;
  public static method Main(): int
    var a := new R();
    var b := a.F;
    var c := a.Take();
    using var t := Domain.Spawn<Q>(W, move c);
    return b.M();
  end Main;
end Program;
