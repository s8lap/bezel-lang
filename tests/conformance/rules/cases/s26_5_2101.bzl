//! case: s26_5_2101
//! doc: spec
//! section: 26.5
//! title: A user extension on CancellationToken is found for a token value
//!   and for a source's Token
//! rule: If the receiver's type declares no instance member of that name,
//!   the compiler searches the current module and all imported modules
//! cites: 26.1, 26.5
//! expect: run
//! output: 2
//!   2
//! issue: 8251
//! notes: Red on base: the pre-fix compiler rejected a user extension
//!   method called on a CancellationToken receiver with 'produced no value,
//!   and no dispatch arm emitted any code'. It passes on the staging tip.
module M;
import System.Threading;

public extension CancellationToken
  public method Twice(): int => 2;
end extension;

public class Program
  public static method Main(): int
    Console.WriteLine(CancellationToken.None.Twice());
    using var cts := new CancellationTokenSource();
    Console.WriteLine(cts.Token.Twice());
    return 0;
  end Main;
end Program;
