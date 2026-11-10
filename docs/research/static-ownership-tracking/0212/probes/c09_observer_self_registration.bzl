module Probe;
// CRITIQUE NOTE 10: observer self-registration. A constructor hands `this` to a longer-lived object that keeps it. EXPECT today: compiles; a report line at `Bus.Subscribe`'s store; checked = DisposedAccessException when the bus notifies after the observer died. RESULT on r40: compiles with NO report line (the note's k2a gap), faults in checked mode, segfaults unchecked. Under 0212: no static form — `bus.Subscribe(scoped this)` is a store into an object the constructor does not hold, and the bus is declared before the observer. c10 is what users write instead.
public class Bus
  private owned _seen: Array<Observer> := new Array<Observer>();
  public constructor()
  end constructor;
  public method Subscribe(o: Observer) => _seen.Add(o);
  public method Notify()
    for var o in _seen do
      Console.WriteLine("notify " + o.Name);
    end;
  end Notify;
end Bus;
public class Observer
  private _name: string;
  public constructor(name: string, bus: Bus)
    _name := name;
    bus.Subscribe(this);
  end constructor;
  public property Name: string => _name;
end Observer;
public class Program
  public static method Main(): int
    using var bus := new Bus();
    do
      using var o := new Observer("o1", bus);
      bus.Notify();
    end;
    bus.Notify();
    return 0;
  end Main;
end Program;
