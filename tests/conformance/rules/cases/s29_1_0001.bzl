//! case: s29_1_0001
//! doc: spec
//! section: 29.1
//! title: a user-implemented IEnumerable/IEnumerator pair drives a for loop
//! rule: `for` operates on any value whose type implements `IEnumerable<T>`.
//!   Calling `GetEnumerator()` produces a fresh enumerator positioned just
//!   before the first element; `MoveNext()` advances the cursor; `Current`
//!   returns the element at the current position. An enumerator with no
//!   resources to release simply has an empty `Dispose` body; the compiler
//!   generates one if the implementor writes none.
//! cites: 29.1
//! expect: run
//! stdout: 6
module Conf;

public class TrioEnumerator: IEnumerator<int>
  private _a: int;
  private _b: int;
  private _c: int;
  private _index: int := -1;

  public constructor(a: int, b: int, c: int)
    _a := a;
    _b := b;
    _c := c;
  end constructor;

  public method MoveNext(): bool
    _index := _index + 1;
    return _index < 3;
  end MoveNext;

  public property Current: int => (case _index of 0 => _a, 1 => _b, _ => _c);
end TrioEnumerator;

public class Trio: IEnumerable<int>
  private _a: int;
  private _b: int;
  private _c: int;

  public constructor(a: int, b: int, c: int)
    _a := a;
    _b := b;
    _c := c;
  end constructor;

  public method GetEnumerator(): owned IEnumerator<int>
    return new TrioEnumerator(_a, _b, _c);
  end GetEnumerator;
end Trio;

public class Program
  public static method Main()
    var total := 0;
    for var x in new Trio(1, 2, 3) do
      total := total + x;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
