case temperature of
  var t when t > 30 => Console.WriteLine($"Hot: {t}"),
  var t when t < 0 => Console.WriteLine($"Freezing: {t}"),
  var t => Console.WriteLine($"Mild: {t}")
end;
