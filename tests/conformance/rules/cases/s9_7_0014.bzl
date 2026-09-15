//! case: s9_7_0014
//! doc: spec
//! section: 9.7
//! title: a value whose every reference-typed field is null satisfies the quantifier
//! rule: The quantifier is over reference-typed fields, not over borrow-carrying
//!   fields alone, so a null-source value holds no reference of any kind. At
//!   `Name: string?`, `new Tok(null, null)` is admitted as a static.
//! cites: 22.12, 28.9
//! expect: accept
module Conf;

public record Tok(Src: Object?, Name: string?);

public class Registry
  public static readonly Missing: Tok := new Tok(null, null);
end Registry;
