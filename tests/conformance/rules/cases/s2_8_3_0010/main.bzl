//! case: s2_8_3_0010
//! doc: spec
//! title: a value of one module's type of a name is not convertible to another module's type
//! section: 2.8.3
//! rule: A type is identified program-wide by its declaring module name, its
//!   qualified type name, and its generic arity. The two `Box` types of `L1`
//!   and `L2` are two types, so a value of one is not convertible to a target
//!   of the other.
//! cites: 2.8.3, 5.1
//! expect: reject
//! diagnostic: not implicitly convertible
module App;
import L1;
import L2;

public class Program
  public static method Main()
    using var a := new L1.Box();
    using var b: L2.Box := a;
    Console.WriteLine(b.Tag());
  end Main;
end Program;
