//! case: s9_7_0004
//! doc: spec
//! section: 9.7
//! title: a record whose only reference-typed field is a string is not borrow-bearing
//! rule: A record whose only reference-typed fields are of declared
//!   runtime-managed types (string, function types) is not a borrow-bearing
//!   record; runtime-managed values have unbounded lifetime by construction, so
//!   no borrow-lifetime rules apply.
//! expect: accept
module Conf;

public record Label
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Label;

public class Registry
  public static readonly Default: Label := new Label("anonymous");
end Registry;
