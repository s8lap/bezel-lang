//! case: s22_5_0033
//! doc: spec
//! section: 22.5
//! title: Semaphore with only maxCount named leaves initialCount unbound
//! rule: Each supplied argument binds exactly one parameter and each parameter is bound by at most one
//!   supplied argument; every parameter that declares no default must be bound (spec 22.5). `Semaphore(initialCount, maxCount)` declares no default for `initialCount`.
//! cites: 22.5, 22.6
//! expect: reject
//! diagnostic: not supplied
module Conf;

import System.Serialization;
import System.Threading;

public class Program
  public static method Main()
    using var w := new BinarySerializer();
    w.WriteInt(1);
    using var bytes := w.ToBytes();
    using var x := new Semaphore(maxCount := 5);
  end Main;
end Program;
