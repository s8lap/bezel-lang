//! case: s28_14_9908
//! doc: spec
//! section: 28.14
//! title: a scoped handler forwarded through a borrowed Holder parameter's field is rejected
//! rule: forward it to another `scoped` parameter whose bounding object is the same object
//! cites: 28.14
//! expect: reject
//! diagnostic: scoped
//! issue: 9274
//! notes: Harvested from the #9274 review (h_param_field_add). A parameter is not `this` or a local
//!   binding, so the chain `h.bus` resolves no bounding object the callee shares.
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
  public method On(h: Holder, scoped handler: (double) -> ())
    h.bus.Add(handler);
  end On;
end Hub;

public class Program
  public static method Main()
    using var keep := new Holder();
    for var i in 0 ..< 200 do
      do
        using var c := new Counter2();
        using var hub := new Hub();
        hub.On(keep, lambda(t) => c.Hit(t));
      end;
      keep.Fire(1.0);
    end for;
    Console.WriteLine("done");
  end Main;
end Program;
