//! case: s10_5_0003
//! doc: spec
//! section: 10.5
//! title: an enum cannot declare a block-form property
//! rule: An enum may declare exactly instance methods, expression-bodied
//!   read-only instance properties, static methods, `static readonly`
//!   fields, `const` members, and operator overloads — and no others; an
//!   enum may not declare block-form properties or properties with backing
//!   storage.
//! cites: 10.4, 10.5
//! expect: reject
//! diagnostic: block-form
module Conf;

enum Priority
  Low, Medium, High;

  public property Label: string
    get => "x";
  end Label;
end Priority;
