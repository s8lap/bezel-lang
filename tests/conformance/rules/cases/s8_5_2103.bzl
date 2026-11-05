//! case: s8_5_2103
//! doc: spec
//! section: 8.5
//! title: ToString on the Lock a TryAcquire returns is the qualified name
//!   after the null test
//! rule: For classes: the type's **qualified name** — the
//!   module-path-qualified rendering stdlib
//! cites: 8.5, 6.2
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
    using var l := m.TryAcquire(TimeSpan.FromMilliseconds(10L));
    if l <> null then
      Console.WriteLine(l.ToString());
    end;
    return 0;
  end Main;
end Program;
