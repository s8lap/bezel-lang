import System.Interop;

// C: void bus_subscribe(bus*, void (*cb)(int, void*), void* user);  — RETAINS cb+user
//    void bus_unsubscribe(bus*, void* user);                        — stops delivery
extern "libbus"
  public method bus_subscribe(b: IntPtr, cb: (int, IntPtr) -> (), user: IntPtr): ();
  public method bus_unsubscribe(b: IntPtr, user: IntPtr): ();
end;

public sealed class Counts                        // wholly-owned state: value fields only
  private _seen: int := 0;
  public method Record(code: int)
    _seen := _seen + 1;
  end Record;
end Counts;

public class BusListener
  private static owned readonly _live: Locked<OwningDictionary<long, Counts>>
      := new(new OwningDictionary<long, Counts>());   // §22.12 rule-4 warning suppressed:
                                                      //   warning-suppression set (owned →
                                                      //   §22.13 exit disposal)
  private static readonly _next: AtomicInteger<long> := new(1L);

  private _bus: IntPtr;
  private _token: long;

  public constructor(bus: IntPtr)
    _bus := bus;
    _token := _next.Increment();
    do
      using var g := _live.Lock();
      g.Value.Add(_token, new Counts());
    end;                                          // guard released BEFORE native gets the token
    bus_subscribe(bus, BusListener.OnNote, IntPtr(_token));   // static ref: admissible today
  end;

  private static method OnNote(code: int, user: IntPtr): ()
    var token := long(user);                      // §32.7 explicit conversion
    using var g := _live.Lock();                  // legal on a foreign thread (§30.12/§32.10)
    if g.Value.ContainsKey(token) then
      g.Value[token].Record(code);                // mutate object state — under the lock
    end;                                          // late post-teardown callback: graceful miss
  end OnNote;

  destructor                                      // sync path — see the teardown discipline below
    bus_unsubscribe(_bus, IntPtr(_token));        // 1. native stops delivering (trusted)
    using var g := _live.Lock();                  // 2. barrier: an in-flight callback that
                                                  //    already holds the lock finishes first
    _ := g.Value.Remove(_token);                  // 3. entry removed and disposed under the lock
  end;
end BusListener;
