do
  using var __source := source;
  using var __enumerator := __source.GetOwnedEnumerator();
  while __enumerator.MoveNext() do
    var x := __enumerator.Current;           // x is owned; discharge required
                                              //   on every body-exit edge
    // body
  end;
end;
