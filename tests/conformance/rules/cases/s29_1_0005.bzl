//! case: s29_1_0005
//! doc: spec
//! section: 29.1
//! title: a class implementing both IEnumerable and IEnumerator may return this from GetEnumerator
//! rule: A class implementing both `IEnumerable<T>` and `IEnumerator<T>` may
//!   return `this` from `GetEnumerator()` — the one receiver return the
//!   announcement table's rows 2–3 admit (axiom A15); the disposal of a
//!   self-returned frame runs the object's `Dispose()` and does not reclaim
//!   it.
//! cites: 29.1
//! expect: run
//! stdout: 3
module Conf;

public class CountUp: IEnumerable<int>, IEnumerator<int>
  private _limit: int;
  private _i: int := -1;

  public constructor(limit: int)
    _limit := limit;
  end constructor;

  public method GetEnumerator(): owned IEnumerator<int>
    return this;
  end GetEnumerator;

  public method MoveNext(): bool
    _i := _i + 1;
    return _i < _limit;
  end MoveNext;

  public property Current: int => _i;
end CountUp;

public class Program
  public static method Main()
    var total := 0;
    for var x in new CountUp(3) do
      total := total + 1;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
