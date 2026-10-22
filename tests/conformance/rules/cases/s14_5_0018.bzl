//! case: s14_5_0018
//! doc: spec
//! section: 14.5
//! title: a byte-typed constant and a byte argument agree, so Math.Max infers T as byte
//! rule: All bindings a type parameter receives — across arguments, lambda returns,
//!   and the target — must be the same type. Two different bindings are an
//!   inference failure: a compile error naming the type parameter and both
//!   types, remedied with explicit type arguments or an explicit cast. Constant
//!   expressions contribute their static types (Pick(b, 250) for b: byte is an
//!   error — 250 is an int).
//! cites: 14.5
//! expect: run
//! stdout: 9
module Conf;

public class Program
  public static method Main()
    var b: byte := 5;
    const K: byte := 9;
    Console.WriteLine(Math.Max(b, K));
  end Main;
end Program;
