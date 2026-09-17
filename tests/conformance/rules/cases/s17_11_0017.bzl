//! case: s17_11_0017
//! doc: spec
//! section: 17.11
//! title: default of a record with a non-nullable reference field is rejected
//! rule: "`default(T)` is a compile error for the non-defaultable types: ... A
//!   record any of whose declared fields is not defaultable — a non-nullable
//!   reference type, a choice, an enum with no zero-valued constant, or
//!   (recursively) a record or inline array with a non-defaultable component."
//! cites: 17.11, 9.1
//! expect: reject
//! diagnostic: defaultable
module Conf;

public record Named(Label: string);

public class Program
  public static method Main()
    var n := default(Named);
    Console.WriteLine(n.Label);
  end Main;
end Program;
