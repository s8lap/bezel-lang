//! case: s26_5_2103
//! doc: spec
//! section: 26.5
//! title: An extension named like a CancellationToken member never wins
//!   over the member
//! rule: Adding a real method to a type silently takes priority over an
//!   extension with the same name — by design.
//! cites: 26.5, 6.1
//! expect: run
//! output: False
//!   done
//! issue: 8251
//! notes: Red on base: the pre-fix compiler let the extension
//!   ThrowIfCancellationRequested win over the declared member and printed
//!   EXT-throw. It passes on the staging tip.
module M;

public extension CancellationToken
  public method IsCancellationRequested(): bool => true;
  public method ThrowIfCancellationRequested() => Console.WriteLine("EXT-throw");
end extension;

public class Program
  public static method Main(): int
    var t: CancellationToken := CancellationToken.None;
    Console.WriteLine(t.IsCancellationRequested);
    t.ThrowIfCancellationRequested();
    Console.WriteLine("done");
    return 0;
  end Main;
end Program;
