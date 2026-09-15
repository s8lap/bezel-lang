//! case: l10_5_0001
//! doc: stdlib
//! section: 10.5
//! title: Ptr<T>.Zero compares equal to itself under the intrinsic address equality
//! rule: public static readonly property Zero: Ptr<T>; ... The
//!   intrinsic compares the address representation.
//! cites: 10.5
//! expect: run
//! stdout: True
module Conf;

import System.Interop;

public class Program
  public static method Main()
    Console.WriteLine(Ptr<int>.Zero = Ptr<int>.Zero);
  end Main;
end Program;
