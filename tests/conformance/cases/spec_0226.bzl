do
  using var __source := source;
  using var __enumerator := __source.GetOwnedEnumerator();
  while __enumerator.MoveNext() do
    var x := __enumerator.TakeCurrent();     // x is owned; §29.1 v0.161
                                              //   discharge required on every body-exit edge
    // body
  end;
end;
