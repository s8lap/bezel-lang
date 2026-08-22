extension IEnumerable<T> where T: value
  public method First(): T
    for var item in this do return item; end;
    throw new InvalidOperationException("empty sequence");
  end First;
end;

extension IEnumerable<int>
  public method First(): int
    // a hand-specialized int form
    for var item in this do return item; end;
    throw new InvalidOperationException("empty sequence");
  end First;
end;

var xs: Array<int> := [1, 2, 3];
var f := xs.First();     // ERROR: ambiguous — both substituted heads are IEnumerable<int>
