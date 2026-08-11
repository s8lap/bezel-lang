using var hot := readings.Where(lambda(r) => r.Celsius > 90.0).ToArray();
var n    := hot.Count;                            // a collection property now
var mean := hot.Average(lambda(r) => r.Celsius);  // as many passes as you like
