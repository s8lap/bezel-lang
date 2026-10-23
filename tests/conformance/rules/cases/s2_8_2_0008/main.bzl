//! case: s2_8_2_0008
//! doc: spec
//! section: 2.8.2
//! title: an importer reconstructs a nested type of a non-generic enclosing type
//! rule: The interface of a compiled artifact must be sufficient for a consumer,
//!   given only the artifact, to type-check every well-formed use of the module's
//!   public surface, with full signatures. A nested type is a member of its
//!   enclosing class, and a nested type none of whose enclosing types is generic
//!   is referenced from outside as `Outer.Nested` — so the consumer reconstructs
//!   `Holder.Cell`, calls its instance member, and passes it to an imported
//!   method whose parameter is that nested type.
//! cites: 2.8.2, 11.6
//! expect: run
//! stdout: 10
module App;
import Lib;

public class Program
  public static method Main()
    using var cell := Holder.Make();
    Console.WriteLine(Holder.Id(cell) + cell.Get());
  end Main;
end Program;
