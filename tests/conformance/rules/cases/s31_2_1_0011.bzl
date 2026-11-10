//! case: s31_2_1_0011
//! doc: spec
//! section: 31.2.1
//! title: a binding over an IFailure arm chains as a cause
//! rule: a binding arm over an interface binds `name` at `Exception` … The binding
//!   therefore reads `Message`, is an argument where an `Exception` is
//!   expected, and chains as a cause (§31.4), whatever interface selected it.
//! cites: 31.2.1, 31.4
//! expect: run
//! stdout: 1
//! issue: 8415
module Conf;

public class LoadError: Exception, IFailure
  public constructor(message: string, cause: Exception) base(message, cause);
  end constructor;
end LoadError;

public class Program
  public static method Load(text: string): int
    try
      return int.Parse(text);
    catch
      var f: IFailure => do
        throw new LoadError("cannot load", f);
      end
    end;
  end Load;

  public static method Main()
    try
      _ := Program.Load("12x");
    catch
      var ex: LoadError => Console.WriteLine(ex.CauseChain.Count)
    end;
  end Main;
end Program;
