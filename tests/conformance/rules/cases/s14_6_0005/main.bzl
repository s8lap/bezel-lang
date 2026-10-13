//! case: s14_6_0005
//! doc: spec
//! section: 14.6
//! title: a private helper's edge closing a cross-module expanding cycle is carried to assembly
//! rule: For this check an artifact carries **every edge of the
//!   instantiation-request graph its compilation holds** — the requests of
//!   every declaration it compiles, generic or not, whatever its visibility:
//!   ... and of every internal declaration such an implementation's requests
//!   reach (a `private` helper `H<U>` called at `Box<T>` from an
//!   implementation, whose own body calls an imported interface method at `U`
//!   that a sibling module implements, holds the edge that closes that cycle)
//! cites: 14.6, 2.8.3, 14.2
//! expect: reject
//! diagnostic: cycle
//! issue: 5815
module Conf;
import Conf.Visit;
import Conf.Lhs;
import Conf.Rhs;

public class Program
  public static method Main()
    using var l := new LeftVisitor();
    using var r := new RightVisitor();
    Console.WriteLine(l.M<int>(r));
  end Main;
end Program;
