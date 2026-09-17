//! case: s11_6_0001
//! doc: spec
//! section: 11.6
//! title: a class body may declare nested types with required visibility
//! rule: A class body may declare nested classes, records, enums, and
//!   interfaces. Nested types may be public, internal, protected, or
//!   private, and the visibility keyword is required.
//! cites: 11.6
//! expect: accept
module Conf;

public class Parser
  public record Token(Kind: TokenKind, Text: string, Line: int);

  public enum TokenKind
    Identifier, Number, Operator, EndOfFile;
  end TokenKind;

  public constructor()
  end constructor;
end Parser;
