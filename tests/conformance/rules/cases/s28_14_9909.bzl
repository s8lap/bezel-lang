//! case: s28_14_9909
//! doc: spec
//! section: 28.14
//! title: a scoped handler forwarded through an owned field of this is admitted
//! rule: a call whose receiver is a **member chain** rooted at `this` or a local binding in which every
//!   hop is an **`owned` field**
//! cites: 28.14, 28.5
//! expect: run
//! stdout: 400
//! issue: 9274
//! notes: Harvested from the #9274 review (a_peer_owned_add), the admitting twin of s28_14_9905. The chain
//!   `_peer.bus` is rooted at `this` with an `owned` field hop, so form 4 resolves the bounding
//!   object to `this` (the same object). The caller's `using var hub` bounds the capture of `c`,
//!   declared before it. Each pass raises twice: 2 per pass, 400.
module Conf;

public class Counter2
  private _n: int := 0;

  public method Hit(t: double)
    _n := _n + 1;
  end Hit;

  public property N: int => _n;
end Counter2;

public class Holder
  public owned readonly bus: EventSource<double> := new EventSource<double>();

  public method Fire(t: double)
    bus.Raise(t);
  end Fire;
end Holder;

public class Hub
  private owned readonly _peer: Holder := new Holder();

  public method FireI(t: double)
    _peer.Fire(t);
  end FireI;

  public method On(scoped handler: (double) -> ())
    _peer.bus.Add(handler);
  end On;
end Hub;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0 ..< 200 do
      using var c := new Counter2();
      using var hub := new Hub();
      hub.On(lambda(t) => c.Hit(t));
      hub.FireI(1.0);
      hub.FireI(1.0);
      total := total + c.N;
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
