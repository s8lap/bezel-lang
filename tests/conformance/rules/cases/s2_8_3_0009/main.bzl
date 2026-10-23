//! case: s2_8_3_0009
//! doc: spec
//! section: 2.8.3
//! title: two modules each declaring a public type of the same name declare two unrelated types
//! rule: A type is identified program-wide by its declaring module name, its
//!   qualified type name, and its generic arity. Two modules each declaring a
//!   public type of the same name declare two unrelated types (distinguished in
//!   diagnostics by module qualification) — so `L1.Box` and `L2.Box` are two
//!   types, and no value of one converts to the other.
//! cites: 2.8.3, 5.1, C.13
//! expect: run
//! output: 1
//!   2
module App;
import L1;
import L2;

public class Program
  public static method Main()
    using var a := new L1.Box();
    using var b := new L2.Box();
    Console.WriteLine(a.Tag());
    Console.WriteLine(b.Tag());
  end Main;
end Program;
