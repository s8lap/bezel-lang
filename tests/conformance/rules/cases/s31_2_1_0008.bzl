//! case: s31_2_1_0008
//! doc: spec
//! section: 31.2.1
//! title: an unconstrained generic parameter used as a catch-arm type is rejected
//! rule: A generic type parameter used as a catch arm's type must carry an
//!   `Exception`-derived class bound; a parameter that is unconstrained is a
//!   compile error at the arm, checked at the generic declaration.
//! cites: 31.2.1
//! expect: reject
//! diagnostic: `Exception`-derived
module Conf;

public class Classifier
  public static method Classify<TEx>(action: () -> ()): string
    try
      action();
      return "no throw";
    catch
      var ex: TEx => return "caught";
    end try;
  end Classify;
end Classifier;
