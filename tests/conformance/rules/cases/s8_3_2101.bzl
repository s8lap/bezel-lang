//! case: s8_3_2101
//! doc: spec
//! section: 8.3
//! title: Equals and GetHashCode on a held Lock follow the class defaults:
//!   reference equality, identity hash
//! rule: | Class | Reference equality | Identity-based |
//! cites: 8.3
//! expect: run
//! output: False
//!   True
//!   True
//!   True
//!   True
//!   True
//! issue: 8252
//! notes: Red on base: the pre-fix compiler rejected ToString/Equals on a
//!   Lock with 'not yet implemented ... (tracked #2457)'. It passes on the
//!   staging tip.
module P;
import System.Threading;

public class Program
  public static method Main(): int
    using var m := new Mutex();
    using var m2 := new Mutex();
    using var l := m.Acquire();
    using var l2 := m2.Acquire();
    var o: Object := l;
    Console.WriteLine(l.Equals(l2));
    Console.WriteLine(l.Equals(l));
    Console.WriteLine(l.GetHashCode() = l.GetHashCode());
    Console.WriteLine(o.GetHashCode() = l.GetHashCode());
    Console.WriteLine(o.Equals(l));
    Console.WriteLine(l.Equals(o));
    return 0;
  end Main;
end Program;
