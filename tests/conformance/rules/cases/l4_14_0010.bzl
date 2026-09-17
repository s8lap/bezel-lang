//! case: l4_14_0010
//! doc: stdlib
//! section: 4.14
//! title: move buf[i] is not admitted for value-typed T
//! rule: For value-typed T, move buf[i] is not admitted -- values have no
//!   ownership to transfer, and are read with the plain indexer instead.
//! cites: 4.14
//! expect: reject
//! diagnostic: move
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(1);
    var v := move buf[0];
  end Main;
end Program;
