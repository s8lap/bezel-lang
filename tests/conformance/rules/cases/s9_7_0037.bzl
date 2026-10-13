//! case: s9_7_0037
//! doc: spec
//! section: 9.7
//! title: a borrow-bearing record with a non-null reference-typed field may not be an instance field (record)
//! rule: A borrow-bearing record may not be stored in an instance field of a class,
//!   except when every reference-typed field is null (the null-source form). The
//!   quantifier is over reference-typed fields, not borrow-carrying fields alone:
//!   a field whose declared type is reference-typed but not borrow-carrying —
//!   a string, a function type — must be null too. new Tok(null, "missing")
//!   is rejected even though the borrow-carrying field is null.
//! cites: 22.12, 28.9, #3291
//! expect: reject
//! diagnostic: borrow-bearing record may not be stored in an instance field
//! issue: 5590
module Conf;

public class Src
  public constructor()
  end constructor;
end Src;

// Tok: src is borrow-carrying (Src?), name is reference-typed NOT borrow-carrying (string)
public record Tok(src: Src?, name: string);

public class Container
    private _tok: Tok;

    public constructor()
        // src is null (borrow-carrying field, ok), name is "missing" (reference-typed NOT borrow-carrying — must also be null)
        // Per §9.7: ALL reference-typed fields must be null for null-source form
        _tok := new Tok(null, "missing");
    end constructor;
end Container;

public class Program
    public static method Main()
    end Main;
end Program;
