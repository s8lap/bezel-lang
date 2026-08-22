// glibc x86-64 layout — the raw kernel struct orders fields differently (per-platform declaration):
public record SigAction(
  Handler: IntPtr,          // sa_handler / sa_sigaction (union head — see the unions decline)
  Mask: [16]ulong,          // glibc sigset_t: 1024 bits
  Flags: int,
  Restorer: IntPtr);        // glibc-internal; set to IntPtr.Zero

extern "libc"
  public method sigaction(signum: int, ref act: SigAction, out old: SigAction): int;
end;
