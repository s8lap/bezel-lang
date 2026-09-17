//! case: s10_5_0004
//! doc: spec
//! section: 10.5
//! title: an enum may declare an expression-bodied read-only instance property
//! rule: An enum may declare exactly instance methods, expression-bodied
//!   read-only instance properties, static methods, `static readonly`
//!   fields, `const` members, and operator overloads.
//! cites: 10.4, 10.5
//! expect: accept
module Conf;

enum Priority
  Low, Medium, High;

  public property Label: string => "x";
end Priority;
