method Report(samples: IEnumerable<Sample>)
  var best: Sample? := null;
  for var s in samples do
    if best = null or s.Score > best!.Score then
      best := s;               // ERROR: through the interface, `s` borrows the
    end;                       //   loop's enumerator — disposed when the loop ends
  end;
end Report;
