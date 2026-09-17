//! case: s12_5_0002
//! doc: spec
//! section: 12.5
//! title: a class with an abstract member must itself be abstract or closed
//! rule: A class containing one or more abstract members must itself be
//!   abstract (or closed, §11.7).
//! cites: 12.5, 11.7
//! expect: reject
//! diagnostic: abstract
module Conf;

public class Shape
  public constructor()
  end constructor;

  public abstract method Area(): double;
end Shape;
