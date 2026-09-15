//! case: s8_4_0004
//! doc: spec
//! section: 8.4
//! title: = on a class invokes the overridden Equals
//! rule: For classes, `=` always invokes `Equals`.
//! cites: 8.3, 8.4
//! expect: run
//! stdout: True
module Conf;

public class Money
  private _cents: int;

  public constructor(cents: int)
    _cents := cents;
  end constructor;

  public override method Equals(other: Object?): bool =>
    (if other is Money then (other as Money)._cents = _cents else false);

  public override method GetHashCode(): int => _cents;
end Money;

public class Program
  public static method Main()
    var a := new Money(5);
    var b := new Money(5);
    Console.WriteLine(a = b);
  end Main;
end Program;
