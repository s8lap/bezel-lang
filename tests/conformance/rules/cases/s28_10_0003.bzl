//! case: s28_10_0003
//! doc: spec
//! section: 28.10
//! title: using var admits an if expression whose two arms are both owned producers
//! rule: An expression whose arm-meet is owned — an if expression over
//!   fresh producers — is an owned producer for using var's admission.
//! cites: 28.10, 28.3
//! expect: accept
module Conf;

public class Buffer
  public constructor()
  end constructor;

  public method CloneBuffer(): owned Buffer
    return new Buffer();
  end CloneBuffer;
end Buffer;

public class Program
  public method Run(useFresh: bool, source: Buffer)
    using var x := (if useFresh then new Buffer() else source.CloneBuffer());
  end Run;
end Program;
