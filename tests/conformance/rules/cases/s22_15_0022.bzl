//! case: s22_15_0022
//! doc: spec
//! section: 22.15
//! title: a pure body's for loop over an interface-typed source is rejected
//! rule: A source whose static type is an interface (`IEnumerable<T>`
//!   included) or a generic type parameter remains rejected: the remedy
//!   is declaring the source at a concrete sequence type.
//! cites: 22.15, 29.2
//! expect: reject
//! diagnostic: concrete
module Conf;

public class Ops
  public static pure method Sum(xs: IEnumerable<int>): int
    var total := 0;
    for var x in xs do
      total := total + x;
    end;
    return total;
  end Sum;
end Ops;
