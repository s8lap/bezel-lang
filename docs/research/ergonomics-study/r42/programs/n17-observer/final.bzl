module Home.Thermo;

// r42: forwarding a scoped handler from a Thermostat method into its EventSource field aborts with
// "refcount over-release (closure)" (v2/v3; new). A capturing handler cannot go through the
// IEvent getter either (§28.14). So the event source is owned by the caller and lent to the
// thermostat, and handlers subscribe on that using var directly.
public class Thermostat(target: double, changed: EventSource<double>)
  private _changed: EventSource<double> := changed;
  private _temp: double := 0.0;
  public readonly property Target: double := target;

  public method Set(t: double)
    _temp := t;
    _changed.Raise(t);
  end Set;
end Thermostat;

public class Display
  private _thermo: Thermostat;
  public constructor(thermo: Thermostat)
    _thermo := thermo;
  end constructor;
  public method Show(t: double) => Console.WriteLine($"display: {t} (target {_thermo.Target})");
end Display;

public class Alarm(limit: double)
  private _limit: double := limit;
  public method Check(t: double)
    if t > _limit then Console.WriteLine($"alarm: too hot {t}"); end;
  end Check;
end Alarm;

public class Program
  public static method Main()
    using var alarm := new Alarm(25.0);
    using var changed := new EventSource<double>();
    using var thermo := new Thermostat(20.0, changed);
    using var display := new Display(thermo);
    using var s1 := changed.Subscribe(lambda(t) => display.Show(t));
    var s2 := changed.Subscribe(lambda(t) => alarm.Check(t));
    thermo.Set(21.5);
    thermo.Set(26.0);
    s2.Dispose();
    thermo.Set(27.0);
  end Main;
end Program;
