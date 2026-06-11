public method Echo(owned source: StreamReader): IEnumerable<string>
  while not source.AtEnd do
    yield return source.ReadLine();
  end;
  // No using var; source is disposed by the iterator state machine
  // when the consumer disposes the enumerator.
end Echo;
