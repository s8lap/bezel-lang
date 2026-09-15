//! case: s15_6_0003
//! doc: spec
//! section: 15.6
//! title: slicing is not supported on inline arrays
//! rule: Slicing is not supported on inline arrays in this version — the
//!   result type `[K]T` would need to encode its length, and K is not a
//!   compile-time constant here (§15.6).
//! cites: 15.6
//! expect: reject
//! diagnostic: slicing
module Conf;

public class Program
  public static method Main()
    var arr: [4]int := [1, 2, 3, 4];
    var s := arr[1..2];
  end Main;
end Program;
