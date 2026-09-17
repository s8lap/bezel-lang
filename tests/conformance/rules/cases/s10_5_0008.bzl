//! case: s10_5_0008
//! doc: spec
//! section: 10.5
//! title: overloading homogeneous = on an enum together with Equals(T) and GetHashCode is admitted
//! rule: A record/enum/choice overloading homogeneous `=`/`<>` also declares
//!   `Equals(T)` and `GetHashCode` — on an enum, `override` or plain (the
//!   co-declaration rule).
//! cites: 10.5, C.10, 25.2
//! expect: accept
module Conf;

enum Priority
  Low, Medium, High;

  public static method operator =(a: Priority, b: Priority): bool => int(a) = int(b);
  public static method operator <>(a: Priority, b: Priority): bool => int(a) <> int(b);
  public method Equals(other: Priority): bool => int(this) = int(other);
  public method GetHashCode(): int => int(this);
end Priority;
