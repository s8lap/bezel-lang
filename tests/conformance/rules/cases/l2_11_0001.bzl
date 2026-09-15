//! case: l2_11_0001
//! doc: stdlib
//! section: 2.11
//! title: Subscription, obtained from a registration call, disposes with using var
//! rule: Subscription has no public constructor and is obtained from a
//!   registration call, such as CancellationToken.Register; it is auto-imported,
//!   needing no explicit import statement.
//! cites: 6.1
//! expect: run
//! stdout: registered
module Conf;

public class Program
  public static method Main()
    var token := CancellationToken.None;
    using var sub: Subscription := token.Register(lambda() => ());
    Console.WriteLine("registered");
  end Main;
end Program;
