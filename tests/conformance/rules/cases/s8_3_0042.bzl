//! case: s8_3_0042
//! doc: spec
//! section: 8.3
//! title: a plain Equals(Object?) on a record is rejected at the declaration
//! rule: On a record, an `Equals` at `Object?` is a compile error at its
//!   declaration, plain or `override`, and the diagnostic points at the
//!   own-typed spelling; the value kinds' Object slot is filled only by the
//!   own-typed `Equals` (v0.338, §9.2).
//! cites: 9.2, 8.3
//! expect: reject
//! diagnostic: may not take
//! issue: 5955
module Conf;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: Object?): bool => true;
end R;
class Program public static method Main(): int Console.WriteLine("ok"); return 0; end Main; end Program;
