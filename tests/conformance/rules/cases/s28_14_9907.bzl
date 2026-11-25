//! case: s28_14_9907
//! doc: spec
//! section: 28.14
//! title: a scoped handler subscribed with += on a static owned field's chain is rejected
//! rule: forward it to another `scoped` parameter whose bounding object is the same object
//! cites: 28.14, 22.13
//! expect: reject
//! diagnostic: scoped
//! issue: 9274
//! notes: Harvested from the #9274 review (h_staticfield_field_pluseq). The `+=` spelling of the same
//!   forward; stdlib 6.5's event pattern bounds by `this` only for an owned instance field.
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
  private static owned readonly _sh: Holder := new Holder();

  public static method Fire(t: double)
    _sh.Fire(t);
  end Fire;

  public method On(scoped handler: (double) -> ())
    _sh.bus += handler;
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
