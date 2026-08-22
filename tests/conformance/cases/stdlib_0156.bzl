/// Static intrinsics for the FFI boundary. Module System.Interop (NOT auto-imported).
public static class Interop
  // The calling thread's C errno, read directly. Valid only when read BEFORE the
  // next extern call or runtime operation on this thread — and NEVER across an
  // await: a continuation may resume on a DIFFERENT OS thread (language spec
  // §30.13), so a post-await read is another thread's errno. Read it first,
  // synchronously, in the same continuation as the failing call.
  public static method LastErrno(): int;               // v0.186

  // The process-lifetime code address of a callback admissible under the STATIC
  // rule of language spec §32.8; F gated per-instantiation to FFI-compatible
  // function types. One-way by design: no address→function inverse exists.
  public static method CodeAddress<F>(callback: F): IntPtr;   // v0.186
end Interop;
