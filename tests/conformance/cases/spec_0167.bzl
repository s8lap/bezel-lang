extension IEnumerable<T>
  public method First(): T
    for var item in this do return item; end;
    throw new InvalidOperationException("empty sequence");
  end First;
end;

extension IEnumerable<int>
  public method First(): int
    // optimized for int (an implementation may, for example,
    // short-circuit on the `Array<int>` storage layout).
    for var item in this do return item; end;
    throw new InvalidOperationException("empty sequence");
  end First;
end;

var xs: Array<int> := [1, 2, 3];
var s := xs.First();                          // selects the IEnumerable<int> overload
