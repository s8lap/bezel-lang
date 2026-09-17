//! case: s21_10_0008
//! doc: spec
//! section: 21.10
//! title: a local type may not appear in a member signature
//! rule: The name may not appear in any member signature of an ENCLOSING or
//!   SIBLING declaration — parameter, return, field/property/indexer,
//!   generic-constraint, or base/interface positions — which is a compile error
//!   naming that rule ("a method-local type may not appear in a member
//!   signature"), v0.301.
//! cites: 21.10, C.1
//! expect: reject
//! diagnostic: member signature
module Conf;

public class Program
  public static method Build(): string
    record Row(Name: string);
    var r := new Row("a");
    return r.Name;
  end Build;

  public static method Show(row: Row)
  end Show;
end Program;
