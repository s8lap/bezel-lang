//! case: s31_2_0014
//! doc: spec
//! section: 31.2
//! title: a failure raised by a finally supersedes a defect and is taken by an IFailure arm
//! rule: A throw escaping a `finally` supersedes the in-flight exception: when a
//!   `finally` entered during exception propagation itself completes by
//!   throwing (directly, or via a call that propagates), the new exception
//!   replaces the one being handled … The ownership of a thrown exception
//!   (§31.1), the cleanup an exception edge runs (K3.15), a task's fault
//!   (§30.9), the purity carve-out (§22.15), supersession (§31.2), and the
//!   reports of §32.8 and §33.3 are the same for both kinds and for
//!   cancellation.
//! cites: 31.2, 31.4
//! expect: run
//! stdout: 1
//! issue: 8414
module Conf;

public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;

public class Program
  public static method Main()
    try
      try
        throw new InvalidOperationException("the bug");
      finally
        throw new ConfigError("cleanup failed");
      end try;
    catch
      var f: IFailure => Console.WriteLine(f.SupersededChain.Count)
    end try;
  end Main;
end Program;
