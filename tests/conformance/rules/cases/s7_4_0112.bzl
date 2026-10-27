//! case: s7_4_0112
//! doc: spec
//! section: 7.4
//! title: a member access on a nullable property is rejected
//! rule: The compiler narrows local variables, parameters other than `ref`
//!   parameters, and `readonly` fields in the tracked shape below; these are
//!   the narrowing carriers. It never narrows mutable fields, properties, or
//!   `ref` parameters.
//! cites: 7.4
//! expect: reject
//! diagnostic: never narrow
module Conf;

public interface I
  property S: string?;
end I;

public class Impl: I
  public property S: string? := "x";
end Impl;

public class Program
  public static method Main(): int
    var i: I := new Impl();
    var t: string := i.S.Trim();
    Console.WriteLine(t.Length);
    return 0;
  end Main;
end Program;
