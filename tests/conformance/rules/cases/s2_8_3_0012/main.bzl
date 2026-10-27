//! case: s2_8_3_0012
//! doc: spec
//! title: a module's body reference to a transitive type pulls that artifact into the build
//! section: 2.8.3
//! rule: The build supplies the transitive closure. A build must present the
//!   artifacts of the root compilation's direct imports and, transitively, of
//!   every module those artifacts' interfaces reference — where an interface's
//!   references include those made by the declaration bodies it carries, not
//!   only its public signatures. `App` imports only `B`; `B`'s body names
//!   `G<int>.N`, so `A`'s artifact must be presented to `B`'s compilation.
//! cites: 2.8.3, 11.6
//! expect: run
//! stdout: 9
module App;
import B;

public class Program
  public static method Main()
    Console.WriteLine(W.Go());
  end Main;
end Program;
