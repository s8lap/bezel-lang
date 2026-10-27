//! case: s2_8_2_0010
//! doc: spec
//! title: an importer reconstructs a nested type of a generic enclosing type with two type arguments
//! section: 2.8.2
//! rule: The interface of a compiled artifact must be sufficient for a consumer,
//!   given only the artifact, to type-check every well-formed use of the module's
//!   public surface, with full signatures. A nested type is a member of its
//!   enclosing class, and the enclosing type's own type arguments are written at
//!   the chain head — so the consumer reconstructs `Pair<int, string>`'s `Item`
//!   and reaches it through the enclosing type's static member.
//! cites: 2.8.2, 11.6
//! expect: run
//! stdout: 11
module App;
import Lib;

public class Program
  public static method Main()
    using var item := Pair<int, string>.Make();
    Console.WriteLine(item.Get());
  end Main;
end Program;
