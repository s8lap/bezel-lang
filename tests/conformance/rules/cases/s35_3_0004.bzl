//! case: s35_3_0004
//! doc: spec
//! section: 35.3
//! title: generic choice construction omitting type arguments is rejected
//! rule: For a generic choice the type arguments are written at the construction; the
//!   declared type supplies the choice type and all its type arguments whole — the
//!   generic type with no arguments (Result.Ok(v)) remains an error.
//! expect: reject
//! diagnostic: type arguments
module Conf;

public class Program
  public static method Main()
    var r := Result.Ok(4);   // ERROR: Result requires its type arguments written at construction
  end Main;
end Program;
