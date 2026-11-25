//! case: s27_8_9860
//! doc: spec
//! section: 27.8
//! title: ReferenceEquals through a class alias, an alias of an alias and a generic alias compares identity
//! rule: see through to the aliased type for identity, equality, method dispatch, and overload resolution
//! cites: 27.8, 8.1, 11.7
//! expect: run
//! output: True
//!   False
//!   True
//!   False
//!   True
//!   False
//!   True
//! issue: 9021
//! notes: Harvested from the #9021 review (r43-rev-m151-9021, l01_legal_core). An alias is a naming
//!   convenience, not a separate type kind (27.8), so `KA.ReferenceEquals` is the root's static as
//!   `K.ReferenceEquals` is (11.7: inherited by every class), and the two-argument call compares
//!   identity (8.1). Lines: KA same / different instance, KB (alias of KA) same / different, GA
//!   (alias of G<int>) same / different, and KA over a null and the null literal.
module Conf;

public class K end K;
public class G<T> end G;
type KA := K;
type KB := KA;
type GA := G<int>;

public class Program
  public static method Main(): int
    var a := new K();
    var b := new K();
    var n: K? := null;
    var g1 := new G<int>();
    var g2 := new G<int>();
    Console.WriteLine(KA.ReferenceEquals(a, a));
    Console.WriteLine(KA.ReferenceEquals(a, b));
    Console.WriteLine(KB.ReferenceEquals(a, a));
    Console.WriteLine(KB.ReferenceEquals(a, b));
    Console.WriteLine(GA.ReferenceEquals(g1, g1));
    Console.WriteLine(GA.ReferenceEquals(g1, g2));
    Console.WriteLine(KA.ReferenceEquals(n, null));
    return 0;
  end Main;
end Program;
