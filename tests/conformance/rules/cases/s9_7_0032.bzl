//! case: s9_7_0032
//! doc: spec
//! section: 9.7
//! title: a borrow-bearing choice with a non-null reference-typed payload may not be stored in an instance field (choice)
//! rule: A borrow-bearing choice may not be stored in an instance field of a class,
//!   except when every reference-typed payload is null (the null-source form). The
//!   quantifier is over reference-typed payloads, not borrow-carrying payloads alone:
//!   a payload whose declared type is reference-typed but not borrow-carrying —
//!   a string, a function type — must be null too. Tagged.Named("a", null)
//!   is rejected even though the borrow-carrying payload is null.
//! cites: 22.12, 28.9, 35.4, #3291
//! expect: reject
//! diagnostic: borrow-bearing choice may not be stored in an instance field
//! issue: 5590
module Conf;

public class Src
  public constructor()
  end constructor;
end Src;

// Tagged with Named case: name (string — reference-typed NOT borrow-carrying),
// src (Src? — borrow-carrying)
public choice Tagged
  Named(name: string, src: Src?),
  Missing;
end Tagged;

public class Container
    private _tag: Tagged?;

    public constructor()
        // name is "a" (reference-typed NOT borrow-carrying — must be null),
        // src is null (borrow-carrying payload, ok)
        // Per §9.7: ALL reference-typed payloads must be null for null-source form
        _tag := Tagged.Named("a", null);
    end constructor;
end Container;

public class Program
    public static method Main()
    end Main;
end Program;
