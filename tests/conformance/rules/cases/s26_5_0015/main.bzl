//! case: s26_5_0015
//! doc: spec
//! section: 26.5
//! title: two equally-specific extensions from different imported modules are ambiguous
//! rule: If two equally-specific extensions match, the call is a compile
//!   error (rule 4). Nothing in the same module can conflict this way
//!   (§26.6 merges same-module blocks and rejects duplicate *signatures*);
//!   the ambiguity here comes from two distinct imported modules each
//!   declaring the same substituted head and name.
//! cites: 26.5, C.13
//! expect: reject
//! diagnostic: Describe
module App;
import Shapes;
import ExtA;
import ExtB;

public class Program
  public static method Main()
    using var s := new Shape();
    Console.WriteLine(s.Describe());
  end Main;
end Program;
