//! case: s26_5_2104
//! doc: spec
//! section: 26.5
//! title: An extension on a user record that is itself named
//!   CancellationToken is found
//! rule: If the receiver's type declares no instance member of that name,
//!   the compiler searches the current module and all imported modules
//! cites: 26.5
//! expect: run
//! output: 2
//! notes: Green on base and on the staging tip: the control that the stdlib
//!   CancellationToken extension fix does not disturb a user type of the
//!   same name.
module M;

public record CancellationToken
  public Id: int;
end CancellationToken;

public extension CancellationToken
  public method Twice(): int => 2;
end extension;

public class Program
  public static method Main(): int
    var t := new CancellationToken(1);
    Console.WriteLine(t.Twice());
    return 0;
  end Main;
end Program;
