do
  using var __source := source;
  using var __enumerator := __source.GetEnumerator();
  while __enumerator.MoveNext() do
    var x := __enumerator.Current;
    // body
  end;
end;
