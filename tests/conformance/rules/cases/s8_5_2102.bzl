//! case: s8_5_2102
//! doc: spec
//! section: 8.5
//! title: A Lock renders through interpolation and string concatenation as
//!   its qualified name
//! rule: For classes: the type's **qualified name** — the
//!   module-path-qualified rendering stdlib
//! cites: 8.5, 18.5
//! expect: run
//! output: [System.Threading.Lock]
//!   xSystem.Threading.Lock
//! issue: 8252
//! notes: Red on base: the pre-fix compiler rejected ToString/Equals on a
//!   Lock with 'not yet implemented ... (tracked #2457)'. It passes on the
//!   staging tip.
module P;
import System.Threading;

public class Program
  public static method Main(): int
    using var m := new Mutex();
    using var l := m.Acquire();
    Console.WriteLine($"[{l}]");
    var s: string := "x" + l.ToString();
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
