//! case: s11_6_0004
//! doc: spec
//! section: 11.6
//! title: a nested class may access the private members of an enclosing instance
//! rule: A nested class has access to all members (including `private`) of
//!   every class enclosing it, at any depth: an instance member through an
//!   instance of that class it holds
//! cites: 11.6
//! expect: accept
module Conf;

public class Outer
  private _secret: int := 42;

  public class Inner
    public constructor()
    end constructor;

    public method Reveal(o: Outer): int => o._secret;
  end Inner;

  public constructor()
  end constructor;
end Outer;
