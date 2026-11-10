module Probe;
// CRITIQUE NOTE 10, what users write instead: the bus exposes an event (stdlib 6.5); the observer keeps the `Subscription` in an `owned` field, so its own disposal severs the registration (spec 28.14, bounding form 3). EXPECT: compiles; no report line; clean in both modes; the second Raise reaches no handler.
public class Bus
  private owned _changed: EventSource<string> := new EventSource<string>();
  public constructor()
  end constructor;
  public property Changed: IEvent<string> => _changed;
  public method Notify(what: string) => _changed.Raise(what);
end Bus;
public class Observer
  private _name: string;
  private owned _sub: Subscription;
  public constructor(name: string, bus: Bus)
    _name := name;
    _sub := bus.Changed.Subscribe(lambda(what: string) => Console.WriteLine(_name + " saw " + what));
  end constructor;
end Observer;
public class Program
  public static method Main(): int
    using var bus := new Bus();
    do
      using var o := new Observer("o1", bus);
      bus.Notify("first");
    end;
    bus.Notify("second");
    Console.WriteLine("done");
    return 0;
  end Main;
end Program;
