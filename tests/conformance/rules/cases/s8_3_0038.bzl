//! case: s8_3_0038
//! doc: spec
//! section: 8.3
//! title: a record nested in a class is held to the GetHashCode obligation
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
class O
  public record R public property V: int; public constructor(v: int) V := v; end constructor;
    public method Equals(o: R): bool => true;
  end R;
end O;
class Program public static method Main(): int return 0; end Main; end Program;
