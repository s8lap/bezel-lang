//! case: s2_8_2_0001
//! doc: spec
//! section: 2.8.2
//! title: a public generic works across modules even when its body reaches an internal helper
//! rule: Visibility inside a generic body is judged at the definition site: a
//!   public generic's body may reference its own module's `internal`
//!   declarations, and an external instantiation binds to them; the artifact
//!   interface must be sufficient for the consumer to type-check and
//!   instantiate the public generic with no access to `Helper`.
//! cites: 2.8.2
//! expect: run
//! stdout: 42
module App;
import Lib;

public class Program
  public static method Main()
    using var b := Doubler.Wrap(21);
    Console.WriteLine(b.Value);
  end Main;
end Program;
