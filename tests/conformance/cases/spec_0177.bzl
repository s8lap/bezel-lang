extension IEnumerable<T> where T: value
  // where T: value — the for-based body returns the iteration variable, which
  // at a class or interface substitution would be an enumerator-sourced borrow
  // that may not escape the loop (§28.3, §29.2, v0.235); at a value substitution
  // that carries no borrow the return copies the value; at a borrow-bearing
  // one the copy's borrows are sourced, as the variable is, at the loop's
  // implicit enumerator, and §9.7's return bullet rejects the instantiation.
  public method First(): T
    for var item in this do return item; end;
    throw new InvalidOperationException("empty sequence");
  end First;
end;

extension Array<T>
  public method First(): T
    if this.Count = 0 then throw new InvalidOperationException("empty array"); end;
    return this[0];
  end First;
end;

var xs: Array<int> := [1, 2, 3];
var f := xs.First();     // selects the Array<T> member — substituted head Array<int>
                         // is a subtype of substituted head IEnumerable<int>
