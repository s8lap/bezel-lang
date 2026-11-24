// bug: NB13
// title: forwarding a `scoped` handler into an `EventSource` field over-releases the closure
// issue: new (closure over-release family: #6269, #8674, #7772)
// rests-on: spec §28.14: a `scoped` function-typed parameter may be forwarded to a same-bound `scoped` parameter; stdlib §6.5 `EventSource<T>.Subscribe(scoped handler)`
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 1
module P;
public class Hub
  private owned readonly _changed: EventSource<double> := new EventSource<double>();
  public method OnChanged(scoped handler: (double) -> ()): owned Subscription => _changed.Subscribe(handler);
  public method Fire(t: double) => _changed.Raise(t);
end Hub;
public class Counter2
  private _n: int := 0;
  public method Hit(t: double) => _n := _n + 1;
  public property N: int => _n;
end Counter2;
public class Program
  public static method Main()
    using var c := new Counter2();
    using var hub := new Hub();
    using var sub := hub.OnChanged(lambda(t) => c.Hit(t));
    hub.Fire(1.0);
    Console.WriteLine(c.N);
  end Main;
end Program;
