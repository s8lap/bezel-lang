//! case: s28_3_0340
//! doc: spec
//! section: 28.3
//! title: an owned Object parameter that the callee never discharges is disposed once at the method's end
//! rule: When an owned local — or an `owned` parameter — reaches a *normal-exit*
//!   scope end without having reached a discharge point, and is not in moved-from
//!   state, it is disposed implicitly at that scope end (for an `owned`
//!   parameter, at the end of the method)
//! cites: 28.1, 28.8, 28.3.10
//! expect: run
//! output: 300
//!   200
//! notes: Each of the 100 iterations hands three values to a callee that never touches its owned parameter, so the callee auto-drops it. Take(new Res()) and TakeN(move r) each dispose one Res, so the destructor count is 2 per iteration and 200 in all; the Array<int> passed with TakeN(move a) is disposed too but has no counter. The three calls return 1 each, so the sum is 300. A parameter disposed twice would push the count past 200, and one never disposed would leave it short.
module Conf;

public class Res
  public static n: int := 0;
  public constructor()
  end constructor;
  destructor Res.n := Res.n + 1; end destructor;
end Res;

public class App
  public static method Take(owned o: Object): int
    return 1;
  end Take;

  public static method TakeN(owned o: Object?): int
    return 1;
  end TakeN;

  public static method Main(): int
    var i := 0;
    var t := 0;
    while i < 100 do
      t := t + Take(new Res());
      var r := new Res();
      t := t + TakeN(move r);
      var a := new Array<int>();
      t := t + TakeN(move a);
      i := i + 1;
    end while;
    Console.WriteLine(t);
    Console.WriteLine(Res.n);
    return 0;
  end Main;
end App;
