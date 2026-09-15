//! case: s18_1_0002
//! doc: spec
//! section: 18.1
//! title: an untyped integer literal that exceeds int's range without an L suffix is rejected
//! rule: "The compiler does not silently promote integer literals: in an untyped
//!   context `3_000_000_000` is a compile error without an `L` suffix."
//! cites: 18.1
//! expect: reject
//! diagnostic: suffix
module Conf;

public class Program
  public static method Main()
    var total := 3_000_000_000;
  end Main;
end Program;
