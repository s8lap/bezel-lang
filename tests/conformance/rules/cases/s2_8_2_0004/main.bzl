//! case: s2_8_2_0004
//! doc: spec
//! section: 2.8.2
//! title: a constraint violation at an external instantiation is diagnosed
//! rule: The artifact interface carries generic parameters with their variance
//!   and constraints; the consumer may instantiate a public generic only at
//!   type arguments satisfying them, so a type argument failing a declared
//!   constraint is diagnosed at the external instantiation.
//! cites: 2.8.2, 14.2
//! expect: reject
//! diagnostic: constraint
module App;
import Lib;

public class Program
  public static method Use()
    using var holder := new Holder<string>();
  end Use;
end Program;
