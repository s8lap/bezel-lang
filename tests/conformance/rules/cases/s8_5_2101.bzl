//! case: s8_5_2101
//! doc: spec
//! section: 8.5
//! title: ToString on a held Lock returns the class's qualified name
//! rule: For classes: the type's **qualified name** — the
//!   module-path-qualified rendering stdlib
//! cites: 8.1, 2.13
//! expect: run
//! output: System.Threading.Lock
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
    Console.WriteLine(l.ToString());
    return 0;
  end Main;
end Program;
