public static method Tail(lines: Array<string>, n: int): IEnumerable<string>
  if n <= 0 then return; end;              // ERROR (§29.5): no plain `return` in an iterator body
  ...
end Tail;

public method Items(): IEnumerable<Item>
  return _items;                           // ERROR: an unmarked mint-announcing return may not lend
end Items;

using var hot := readings.Where(lambda(r) => r.Celsius > 90.0);
var n := hot.Count();                      // spends the single enumeration right
for var r in hot do Plot(r); end;          // ERROR: already consumed

method Report(samples: IEnumerable<Sample>)
  var best: Sample? := null;
  for var s in samples do
    if best = null or s.Score > best!.Score then best := s; end;   // ERROR: `s` borrows the enumerator
  end;
end Report;
