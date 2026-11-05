//! case: s26_5_2102
//! doc: spec
//! section: 26.5
//! title: A user extension on CancellationToken is found inside async
//!   methods on a parameter and a local
//! rule: If the receiver's type declares no instance member of that name,
//!   the compiler searches the current module and all imported modules
//! cites: 26.5, 30.8
//! expect: run
//! output: 4
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
  public static async method Work(token: CancellationToken): Task<int>
    return token.Twice();
  end Work;

  public static async method Work2(): Task<int>
    var t := CancellationToken.None;
    return t.Twice();
  end Work2;

  public static method Main(): int
    using var cts := new CancellationTokenSource();
    var x := Program.Work(cts.Token).Result;
    var y := Program.Work2().Result;
    Console.WriteLine(x + y);
    return 0;
  end Main;
end Program;
