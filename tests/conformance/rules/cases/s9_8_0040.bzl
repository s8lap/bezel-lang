//! case: s9_8_0040
//! doc: spec
//! section: 9.8
//! title: a body record with a homogeneous equality overload stays serialization-eligible
//! rule: §9.2's homogeneous-`=`/`<>` disqualifier *and* the matching exclusion
//!   in §9.2's membership list belong to the *equality* package and are not
//!   read here — for this check a homogeneous `=`/`<>` overload is an admitted
//!   member exactly as any other operator overload is, an overloaded `=` saying
//!   how two values compare and nothing about which components serialize. The
//!   `override Equals`/`GetHashCode` disqualifier is read out for the same
//!   reason
//! cites: 9.8, 9.2, 25.2
//! expect: run
//! stdout: Cents=250
module Conf;

import System.Serialization;

public record Money: ISerializable, IDeserializable
  public readonly property Cents: int;

  public constructor(cents: int)
    Cents := cents;
  end constructor;

  public static method operator =(a: Money, b: Money): bool => a.Cents = b.Cents;
  public static method operator <>(a: Money, b: Money): bool => a.Cents <> b.Cents;

  public method Equals(other: Money): bool => Cents = other.Cents;
  public override method GetHashCode(): int => Cents;
end Money;

public class Program
  public static method Main()
    var m := new Money(250);
    using var s := new JsonSerializer();
    m.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    _ := d.BeginObject();
    var n := d.ReadFieldName();
    var v := d.ReadInt();
    d.EndObject();
    Console.WriteLine($"{n}={v}");
  end Main;
end Program;
