//! case: s28_14_9910
//! doc: spec
//! section: 28.14
//! title: a scoped handler forwarded through a two-hop owned chain of this is admitted
//! rule: a call whose receiver is a **member chain** rooted at `this` or a local binding in which every
//!   hop is an **`owned` field**
//! cites: 28.14, 28.5
//! expect: run
//! stdout: 400
//! issue: 9274
//! notes: Harvested from the #9274 review (a_peer_owned_2hop). Every hop of `_peer.peer.bus` is an `owned`
//!   field rooted at `this`, so form 4 still resolves `this`. 2 raises per pass: 400.
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

public class Peer
  public owned readonly peer: Holder := new Holder();
end Peer;

public class Hub
  private owned readonly _peer: Peer := new Peer();

  public method FireI(t: double)
    _peer.peer.Fire(t);
  end FireI;

  public method On(scoped handler: (double) -> ())
    _peer.peer.bus.Add(handler);
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
