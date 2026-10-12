//! case: s34_0007
//! doc: spec
//! section: 34
//! title: the triple-slash form is the designated doc comment syntax
//! rule: Three comment forms (§1.5): // ... line comment, /* ... */ block comment,
//!   /// ... documentation comment.
//! expect: accept
module Conf;

/// <summary>Doc comment on a class.</summary>
public class DocSlash
  /** <summary>Block comment as doc comment on a field.</summary> */
  public property X: int;

  // <summary>Plain line comment (not a doc comment).</summary>
  public property Y: int;
end DocSlash;
