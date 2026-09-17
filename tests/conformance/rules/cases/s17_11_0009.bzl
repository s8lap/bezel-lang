//! case: s17_11_0009
//! doc: spec
//! section: 17.11
//! title: default of an enum with no zero-valued constant is rejected
//! rule: "An enum type with no declared constant of underlying value 0.
//!   Every producer of an enum value yields a declared value, and `default`
//!   is no exception: for such an enum, `default(E)` ... [is] rejected."
//! cites: 17.11, 5.4
//! expect: reject
//! diagnostic: defaultable
module Conf;

public enum StatusCode
  Ok := 200, NotFound := 404;
end StatusCode;

public class Program
  public static method Main()
    var s := default(StatusCode);
  end Main;
end Program;
