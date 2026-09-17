//! case: s17_3_0002
//! doc: spec
//! section: 17.3
//! title: a record may overload the = operator
//! rule: "`=` and `<>` invoke `Equals` with a null-safe wrapper. For classes,
//!   `=` cannot be overloaded; for records and enums, it can."
//! cites: 17.3, 25.2
//! expect: accept
module Conf;

public record Box(V: int)
  public static method operator =(a: Box, b: Box): bool => a.V = b.V;
  public static method operator <>(a: Box, b: Box): bool => a.V <> b.V;

  public method Equals(other: Box): bool => V = other.V;
  public override method GetHashCode(): int => V.GetHashCode();
end Box;
