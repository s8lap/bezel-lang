//! case: s22_12_8002
//! doc: spec
//! section: 22.12
//! title: ReferenceEquals named through a generic class instantiation compares identity
//! rule: A static member declared on a base class is a member of every class derived from it
//! cites: 22.12, 14.1, 8.1
//! expect: run
//! output: False
//!   True
//! notes: Pins #8795 for the generic type name spelling (a static member of a generic class
//!   is named with its type arguments, 22.12).
module Conf;
public class G<T> end G;
public class Program
  public static method Main(): int
    var g := new G<string>();
    var g2 := new G<string>();
    Console.WriteLine(G<string>.ReferenceEquals(g, g2));
    Console.WriteLine(G<string>.ReferenceEquals(g, g));
    return 0;
  end Main;
end Program;
