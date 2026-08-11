import System.Interop;

// C (POSIX, Linux x86-64): struct timespec { time_t tv_sec; long tv_nsec; };
public record TimeSpec(Sec: long, Nsec: long);          // C-compatible: two longs

extern "libc"
  public method clock_gettime(clockId: int, out ts: TimeSpec): int;
end;

public static class Clock
  public static method MonotonicNanos(): long
    var rc := clock_gettime(1, out var ts);               // CLOCK_MONOTONIC
    if rc <> 0 then
      var e := Interop.LastErrno();                       // read FIRST (§32.9)
      throw new IOException($"clock_gettime failed: errno={e}");
    end;
    return ts.Sec * 1000000000L + ts.Nsec;
  end MonotonicNanos;
end Clock;
