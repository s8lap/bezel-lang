//! case: s2_8_2_0009
//! doc: spec
//! section: 2.8.2
//! title: an importer reconstructs a type nested two levels deep in a generic enclosing type
//! rule: The interface of a compiled artifact must be sufficient for a consumer,
//!   given only the artifact, to type-check every well-formed use of the module's
//!   public surface, with full signatures. Nested types may themselves contain
//!   nested types (§11.6), and the enclosing path is part of a nested type's
//!   qualified name — so the consumer reconstructs `Tree<int>`'s `Node.Leaf` and
//!   reaches it through the enclosing type's static member.
//! cites: 2.8.2, 11.6
//! expect: run
//! stdout: 7
module App;
import Lib;

public class Program
  public static method Main()
    using var leaf := Tree<int>.Make();
    Console.WriteLine(leaf.Get());
  end Main;
end Program;
