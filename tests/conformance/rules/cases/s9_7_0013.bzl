//! case: s9_7_0013
//! doc: spec
//! section: 9.7
//! title: the static null-source quantifier reaches every reference-typed field
//! rule: The quantifier is over reference-typed fields, not over borrow-carrying
//!   fields alone: a field whose declared type is reference-typed but not
//!   borrow-carrying — a `string`, a function type — must be null too.
//!   `record Tok(Src: Object?, Name: string)` initialized `new Tok(null,
//!   "missing")` is therefore rejected.
//! cites: 22.12, 28.9
//! expect: reject
//! diagnostic: reference-typed
module Conf;

public record Tok(Src: Object?, Name: string);

public class Registry
  public static readonly Missing: Tok := new Tok(null, "missing");
end Registry;
