//! case: l10_0_0001
//! doc: stdlib
//! section: 10.0
//! title: IntPtr's explicitly-declared equality compares by address
//! rule: Equality for these types is therefore declared explicitly ...
//!   the = and <> overloads plus the Equals(T) and override
//!   GetHashCode that language spec §25.2's co-declaration rule
//!   requires.
//! cites: 10.0
//! expect: run
//! output: True
//!   True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(IntPtr.Zero = IntPtr.Zero);
    Console.WriteLine(IntPtr.Zero <> IntPtr(1L));
  end Main;
end Program;
