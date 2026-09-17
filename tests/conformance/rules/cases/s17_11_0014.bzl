//! case: s17_11_0014
//! doc: spec
//! section: 17.11
//! title: default(T) under where T: class is rejected
//! rule: "Under `where T: class` (or any type-bound constraint that implies
//!   non-null T per §14.2), `default(T)` is a compile error — the constraint
//!   guarantees T is non-nullable, and the null value `default(T)` would
//!   produce does not fit T's contract."
//! cites: 17.11, 14.2
//! expect: reject
//! diagnostic: default
module Conf;

public class Box<T> where T: class
  public method Try()
    var v := default(T);
  end Try;
end Box;
