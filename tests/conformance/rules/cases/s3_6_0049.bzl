//! case: s3_6_0049
//! doc: spec
//! section: 3.6
//! title: a declared Equals that is false for every pair keeps both elements in a Set
//! rule: A value-like choice outside auto-hashable through its own
//!   declared equality, `Equals(other: C)` with `GetHashCode`, is hashable
//!   on the strength of those members (v0.338, §3.6 hashable row).
//! cites: 3.6, 8.3
//! expect: run
//! output: 2
//! issue: 5997
module Conf;
record Key(X: int)
  public method Equals(other: Key): bool => false;
  public method GetHashCode(): int => 0;
  public static method operator =(a: Key, b: Key): bool => false;
  public static method operator <>(a: Key, b: Key): bool => true;
end Key;
class C public static method Main(): int
  var s := new Set<Key>(); s.Add(new Key(1)); s.Add(new Key(1)); Console.WriteLine(s.Count.ToString());
  return 0;
end Main; end C;
