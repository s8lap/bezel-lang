//! case: l4_12_0001
//! doc: stdlib
//! section: 4.12
//! title: IEnumerator<T>.Current is non-destructive and repeats without an intervening MoveNext
//! rule: IEnumerator<T>.Current is non-destructive -- it returns a borrow of the
//!   most-recently-yielded value, which the enumerator continues to own; reading
//!   it again without an intervening MoveNext yields the same value.
//! cites: 4.12
//! expect: run
//! stdout: 5
module Conf;

public class OneShot: IEnumerator<int>
  private _started: bool := false;

  public constructor()
  end constructor;

  public method MoveNext(): bool
    if _started then
      return false;
    end;
    _started := true;
    return true;
  end MoveNext;

  public property Current: int => 5;
end OneShot;

public class Source: IEnumerable<int>
  public constructor()
  end constructor;

  public method GetEnumerator(): owned IEnumerator<int>
    return new OneShot();
  end GetEnumerator;
end Source;

public class Program
  public static method Main()
    using var e := new Source().GetEnumerator();
    _ := e.MoveNext();
    var first := e.Current;
    var second := e.Current;
    if first = second then
      Console.WriteLine(first);
    end;
  end Main;
end Program;
