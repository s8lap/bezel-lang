//! case: s11_3_0009
//! doc: spec
//! section: 11.3
//! title: methods with different numbers of type parameters of their own never clash
//! rule: "Two methods declaring different numbers of type parameters of their
//!   own never clash, whatever their parameters: `M<T>(x: T)` beside
//!   `M<T, U>(x: T)`."
//! cites: 11.3, 14.5
//! expect: accept
module Conf;

public class Sample
  public constructor()
  end constructor;

  public method M<T>(x: T): string => "one";
  public method M<T, U>(x: T): string => "two";
end Sample;
