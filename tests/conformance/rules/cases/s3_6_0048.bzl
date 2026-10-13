//! case: s3_6_0048
//! doc: spec
//! section: 3.6
//! title: a Set of choices with a declared Equals and GetHashCode holds one element for two values
//! rule: A value-like choice outside auto-hashable through its own
//!   declared equality, `Equals(other: C)` with `GetHashCode`, is hashable
//!   on the strength of those members (v0.338, §3.6 hashable row).
//! cites: 3.6, 8.3, 35.7
//! expect: run
//! output: 1
//! issue: 5997
module Conf;
choice S A(x: int), B;
  public method Equals(o: S): bool => true;
  public method GetHashCode(): int => 0;
  public static method operator =(a: S, b: S): bool => true;
  public static method operator <>(a: S, b: S): bool => false;
end S;
class C public static method Main(): int
  var s := new Set<S>(); s.Add(S.A(1)); s.Add(S.A(2)); Console.WriteLine(s.Count.ToString());
  return 0;
end Main; end C;
