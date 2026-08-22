public method ReadAndClose(owned reader: StreamReader): owned IEnumerable<string>
  using var r := move reader;             // ownership moves: parameter → local
  while not r.AtEnd do
    yield return r.ReadLine();
  end;
  // r disposed by using var when the iterator completes or is disposed early.
end ReadAndClose;
