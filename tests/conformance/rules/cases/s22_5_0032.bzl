//! case: s22_5_0032
//! doc: spec
//! section: 22.5
//! title: BinaryDeserializer with input bound positionally and by name is rejected
//! rule: Each supplied argument binds exactly one parameter and each parameter is bound by at most one
//!   supplied argument; every parameter that declares no default must be bound (spec 22.5). Naming a parameter already bound positionally is a compile error (spec 22.5).
//! cites: 22.5, 22.6
//! expect: reject
//! diagnostic: bound
module Conf;

import System.Serialization;
import System.Threading;

public class Program
  public static method Main()
    using var w := new BinarySerializer();
    w.WriteInt(1);
    using var bytes := w.ToBytes();
    using var x := new BinaryDeserializer(bytes.AsReadOnlySpan(), input := bytes.AsReadOnlySpan());
  end Main;
end Program;
