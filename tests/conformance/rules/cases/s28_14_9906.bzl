//! case: s28_14_9906
//! doc: spec
//! section: 28.14
//! title: a scoped handler forwarded through this-qualified static owned field is rejected
//! rule: forward it to another `scoped` parameter whose bounding object is the same object
//! cites: 28.14, 22.13
//! expect: reject
//! diagnostic: scoped
//! issue: 9274
//! notes: Harvested from the #9274 review (h_this_static_add). Spelling the static field as `this._bus`
//!   does not make it an owned instance field of `this`; the bounding object stays the static, not
//!   the instance.
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
  private static owned readonly _bus: EventSource<double> := new EventSource<double>();

  public static method Fire(t: double)
    _bus.Raise(t);
  end Fire;

  public method On(scoped handler: (double) -> ())
    this._bus.Add(handler);
  end On;
end Hub;

public class Program
  public static method Main()
    for var i in 0 ..< 200 do
      do
        using var c := new Counter2();
        using var hub := new Hub();
        hub.On(lambda(t) => c.Hit(t));
      end;
      Hub.Fire(1.0);
    end for;
    Console.WriteLine("done");
  end Main;
end Program;
