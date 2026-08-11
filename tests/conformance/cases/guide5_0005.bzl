method Report(samples: Array<Sample>)   // concrete: the array is the backing
  var best: Sample? := null;
  for var s in samples do
    if best = null or s.Score > best!.Score then
      best := s;                 // fine: `s` borrows a slot of `samples`,
    end;                         //   which outlives the loop
  end;
  Console.WriteLine(best?.Name ?? "no samples");
end Report;
