//! case: s2_8_3_0011
//! doc: spec
//! title: nested types of the same name under different enclosing types are three unrelated types
//! section: 2.8.3
//! rule: A type is identified program-wide by its declaring module name, its
//!   qualified type name (including any enclosing-type path, each enclosing
//!   type in it keyed with its own generic arity), and its generic arity. The
//!   three nested `N` types — `L1.G<T>.N`, `L2.K<A, B>.N` and `L2.J.N` — are
//!   three unrelated types, and the consumer reaches each of them.
//! cites: 2.8.3, 11.6
//! expect: run
//! stdout: 543
module App;
import L1;
import L2;

public class Program
  public static method Main()
    using var a := G<int>.Make();
    using var b := K<int, string>.Make();
    using var c := J.Make();
    Console.WriteLine(a.Get() + b.Get() + c.Get());
  end Main;
end Program;
