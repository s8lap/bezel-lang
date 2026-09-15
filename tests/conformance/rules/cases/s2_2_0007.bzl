//! case: s2_2_0007
//! doc: spec
//! section: 2.2
//! title: an alias with no colliding declaration is accepted
//! rule: An alias may not collide with a type declared in the file's module;
//!   a distinctly-named type is unaffected.
//! expect: accept
module Conf;
import System.Threading as Th;

public class ThHelper
  public constructor()
  end constructor;
end ThHelper;
