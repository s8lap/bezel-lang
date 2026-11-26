//! case: s28_14_3607
//! doc: spec
//! section: 28.14
//! title: a += forward of a scoped parameter in the callee's own body is admitted
//! rule: the same statement in the callee's own body is admitted as *The `scoped` modifier* above states
//! cites: 28.14, 17.2
//! expect: run
//! stdout: 1
//! issue: 9311
//! notes: Accepting twin of s28_14_3604: the forward leaves the lambda. One raise, one hit.
module Conf;

public class Counter2
  private _n: int := 0;
  public method Hit(t: double)
    _n := _n + 1;
  end Hit;
  public property N: int => _n;
end Counter2;

public class Hub
  private owned readonly _changed: EventSource<double> := new EventSource<double>();

  public method Fire(t: double)
    _changed.Raise(t);
  end Fire;

  public method On(scoped handler: (double) -> ())
    _changed += handler;
  end On;
end Hub;

public class Program
  public static method Main()
    using var c := new Counter2();
    using var hub := new Hub();
    hub.On(lambda(t) => c.Hit(t));
    hub.Fire(1.0);
    Console.WriteLine(c.N);
  end Main;
end Program;
