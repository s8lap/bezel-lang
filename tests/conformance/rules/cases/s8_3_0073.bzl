//! case: s8_3_0073
//! doc: spec
//! section: 8.3
//! title: a value-constrained choice with a parameter payload obliges GetHashCode
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3, 35.4
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
class R public V: int := 0; destructor end destructor; end R;

choice W<T> where T: value Has(v: T), No;
  public method Equals(o: W<T>): bool => true;
end W;
class Program public static method Main(): int return 0; end Main; end Program;
