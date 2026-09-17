//! case: s2_2_0006
//! doc: spec
//! section: 2.2
//! title: an import alias cannot collide with a type declared in the module
//! rule: An alias may not collide with another alias, an imported module's
//!   name segment in qualifier position, a type declared in the file's
//!   module, or an unambiguously imported public bare name.
//! expect: reject
//! diagnostic: alias
module Conf;
import System.Threading as Th;

public class Th
  public constructor()
  end constructor;
end Th;
