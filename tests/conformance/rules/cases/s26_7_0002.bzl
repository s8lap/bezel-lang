//! case: s26_7_0002
//! doc: spec
//! section: 26.7
//! title: an extension may return this as an ordinary receiver-sourced
//!   borrow
//! rule: The receiver is always a borrow; extensions cannot return it as
//!   `owned` — but an ordinary (non-owned) borrow return of the receiver
//!   remains legal, the same receiver-sourced borrow return every instance
//!   or extension method may make.
//! cites: 26.7, 28.3
//! expect: run
//! stdout: kept
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Tag(): string => "kept";
end Widget;

public extension Widget
  public method Keep(): Widget => this;
end;

public class Program
  public static method Main()
    using var w := new Widget();
    Console.WriteLine(w.Keep().Tag());
  end Main;
end Program;
