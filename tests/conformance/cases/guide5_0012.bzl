using var hot := readings.Where(lambda(r) => r.Celsius > 90.0);   // bound lazy query
var n := hot.Count();            // spends its one enumeration right
for var r in hot do              // ERROR: `hot` was already consumed — the
  Plot(r);                       //   diagnostic points at .ToArray()
end;
