//! case: s29_1_0002
//! doc: spec
//! section: 29.1
//! title: a duck-typed enumerable with no IEnumerable conformance is rejected in a for loop
//! rule: `for` operates on any value whose type implements `IEnumerable<T>`.
//!   The protocol is interface-based; there is no duck-typing fallback.
//! cites: 29.1
//! expect: reject
//! diagnostic: does not implement
module Conf;

public class FakeEnumerator
  private _i: int := -1;
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;

  public method MoveNext(): bool
    _i := _i + 1;
    return _i < _n;
  end MoveNext;

  public property Current: int => _i;
end FakeEnumerator;

public class FakeSeq
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;

  public method GetEnumerator(): owned FakeEnumerator
    return new FakeEnumerator(_n);
  end GetEnumerator;
end FakeSeq;

public class Program
  public static method Main()
    for var x in new FakeSeq(3) do
      Console.WriteLine(x);
    end;
  end Main;
end Program;
