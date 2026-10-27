//! case: s2_8_2_0007
//! doc: spec
//! section: 2.8.2
//! title: an importer reconstructs a generic type's nested type and instantiates it
//! rule: The interface of a compiled artifact must be sufficient for a consumer,
//!   given only the artifact, to type-check every well-formed use of the module's
//!   public surface, with full signatures. A nested type is one type whatever the
//!   instantiation of its enclosing type, and is identified by its declaring module,
//!   its qualified name with the enclosing-type path, and its own generic arity —
//!   so `Box<int>.Item<int>` is one type the importer reconstructs, and `Item<int>`
//!   is a distinct instantiation of it.
//! cites: 2.8.2, 2.8.3, 11.6
//! expect: run
//! stdout: 7
module App;
import Lib;

public class Program
  public static method Main()
    using var item := Box<int>.Make();
    Console.WriteLine(item.Get());
  end Main;
end Program;
