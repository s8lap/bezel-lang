do
  using var __source := source;
  using var __enumerator := __source.GetOwningEnumerator();
  while __enumerator.MoveNext() do
    var x := __enumerator.TakeCurrent();     // x is owned; §28.3 rules apply
                                              //   on every body-exit edge
    // body
  end;
end;
