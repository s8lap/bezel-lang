//! case: s22_5_0070
//! doc: spec
//! section: 22.5
//! title: a required parameter left unbound by a named argument is refused
//! rule: **Every parameter that declares no default must be bound**, positionally or by name; leaving one unbound is the opposite disposition from binding one twice — it makes the candidate **inapplicable** ([§22.6](#sec-overloading)), so at an overloaded name an applicable sibling still resolves and the error is reported only where no candidate survives.
//! cites: 22.5, 22.6
//! expect: reject
//! diagnostic: named arguments
//! issue: 7982
module Conf;
import System.IO;

public class Program
  public static method Main(): int
    Directory.Delete(recursive := true);
    return 0;
  end Main;
end Program;
