//! case: s11_3_0010
//! doc: spec
//! section: 11.3
//! title: a non-generic method and a one-parameter generic method with the same parameter types do not clash
//! rule: "Two methods declaring different numbers of type parameters of their
//!   own never clash, whatever their parameters: `FromException<T>(owned
//!   exception: Exception)` beside the non-generic `FromException(owned
//!   exception: Exception)`."
//! cites: 11.3, 14.5
//! expect: accept
module Conf;

public class Sample
  public constructor()
  end constructor;

  public method FromException(owned exception: Exception): string => "non-generic";
  public method FromException<T>(owned exception: Exception): string => "generic";
end Sample;
