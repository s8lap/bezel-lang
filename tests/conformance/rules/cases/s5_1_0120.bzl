//! case: s5_1_0120
//! doc: spec
//! section: 5.1
//! title: a class implementing IEnumerable<int> converts to IEnumerable<V> under an explicit list
//! rule: **a type to an interface it implements or conforms to**: classes by declaration or inheritance
//! cites: 5.1, 11.2, 14.5
//! expect: accept
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

public class Bag: IEnumerable<int>
  public constructor()
  end constructor;

  public method GetEnumerator(): owned IEnumerator<int>
    return new OneShot();
  end GetEnumerator;
end Bag;

public class C
  public method S<V>(x: IEnumerable<V>): int => 10;
end C;

public class Program
  public static method Use(): int
    using var c := new C();
    using var b := new Bag();
    Console.WriteLine(c.S<int>(b));
    return 0;
  end Use;
end Program;
