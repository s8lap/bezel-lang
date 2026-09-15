//! case: s10_5_0007
//! doc: spec
//! section: 10.5
//! title: overloading homogeneous = on an enum without Equals(T)/GetHashCode is rejected
//! rule: A record/enum/choice overloading homogeneous `=`/`<>` also declares
//!   `Equals(T)` and `GetHashCode` — on an enum, `override` or plain (the
//!   co-declaration rule).
//! cites: 10.5, C.10, 25.2
//! expect: reject
//! diagnostic: gethashcode
module Conf;

enum Priority
  Low, Medium, High;

  public static method operator =(a: Priority, b: Priority): bool => int(a) = int(b);
  public static method operator <>(a: Priority, b: Priority): bool => int(a) <> int(b);
end Priority;
