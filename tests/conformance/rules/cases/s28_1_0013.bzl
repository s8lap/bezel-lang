//! case: s28_1_0013
//! doc: spec
//! section: 28.1
//! title: owned on a method parameter of type int? is rejected
//! rule: a primitive, `string`, `Type`, a function type, an ordinary record, a
//!   value-like or borrow-bearing choice (§35.4), an enum, a non-owning inline
//!   array, or the unit type: `owned` is a compile error.
//! cites: 28.1, 28.9
//! expect: reject
//! diagnostic: owned
//! notes: A nullable int is not a class type, an interface type or a bare type parameter, so it carries no disposal obligation to transfer and the marker is an error. Deleting `owned` leaves a valid method; the accepting twin with a class-typed parameter is s28_1_0001.
module Conf;

public class App
  public static method Take(owned o: int?): int
    return 1;
  end Take;

  public static method Main(): int
    return 0;
  end Main;
end App;
