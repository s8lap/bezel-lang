module Home.Thermo;

public class Thermostat(target: double)
  private owned readonly _changed: EventSource<double> := new EventSource<double>();
  private _temp: double := 0.0;
  public readonly property Target: double := target;
  public property Changed: IEvent<double> => _changed;
  // a capturing handler cannot be subscribed through the Changed getter (§28.14: the bounding
  // object must be a using var or this), so forward a scoped parameter
  public method OnChanged(scoped handler: (double) -> ()): owned Subscription => _changed.Subscribe(handler);

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
    using var alarm := new Alarm(25.0);   // §28.14: captured sources are declared before the bounding object
    using var thermo := new Thermostat(20.0);
    using var display := new Display(thermo);
    using var s1 := thermo.OnChanged(lambda(t) => display.Show(t));
    var s2 := thermo.OnChanged(lambda(t) => alarm.Check(t));
    thermo.Set(21.5);
    thermo.Set(26.0);
    s2.Dispose();
    thermo.Set(27.0);
  end Main;
end Program;
