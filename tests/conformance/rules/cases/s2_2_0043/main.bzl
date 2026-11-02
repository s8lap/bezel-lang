//! case: s2_2_0043
//! doc: spec
//! section: 2.2
//! title: an imported generic of an always-available non-generic name is reachable by its bare name
//! rule: For this collision rule and the shadowing rule below, names are compared together with their generic arity, the component [§2.8.3](#sec-transitive-closure-and-type-identity) rule 3 adds to a type's identifier, and every reference fixes that arity by its type-argument list ([§14.1](#sec-generic-declarations) *Instantiation and its site*, [Appendix C.1](#sec-appendix-c-type-system)): a non-generic `Counter` and the always-available `Counter<K>` are two names, neither colliding with nor shadowing the other (so a file importing a library's `Counter` uses both `Counter.Bump()` and `Counter<string>`), while an imported `Counter<T>` and `Counter<K>` collide.
//! cites: 2.2, 2.3, 2.7
//! expect: run
//! output: 7
//! issue: 7994
module App;
import Lib;

public class Program
  public static method Main(): int
    using var t := new Type<int>();
    Console.WriteLine(t.Hi());
    return 0;
  end Main;
end Program;
