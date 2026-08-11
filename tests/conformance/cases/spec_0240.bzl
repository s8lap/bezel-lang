public method Echo(owned source: StreamReader): owned IEnumerable<string>
  while not source.AtEnd do
    yield return source.ReadLine();
  end;
  // No using var: source is disposed by the iterator state machine when the
  // consumer disposes the enumerator. Legal, and carries the §28.3 must-use
  // warning — ReadAndClose above is the warning-free spelling.
end Echo;
