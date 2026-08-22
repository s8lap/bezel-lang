public interface ISharedEvent<out T>
  method Subscribe(handler: (T) -> ()): owned Subscription;
      // capture-free admission: the handler must be a non-capturing lambda or a
      // static method reference — a stdlib-stated position in spec §28.14's
      // capture-free register (the same register Domain.Spawn's entries live in),
      // NOT a `scoped` storage-bounded position.
end ISharedEvent;

public sealed class SharedEventSource<T>: ISharedEvent<T>
  public constructor();
  public method Subscribe(handler: (T) -> ()): owned Subscription;   // capture-free (as above)
  public method Raise(payload: T);       // synchronized snapshot-raise; handlers run on the raising domain
  public method Add(handler: (T) -> ());                             // capture-free (as above)
  public method Remove(handler: (T) -> ());                          // call-bounded: compares only
  destructor;                            // synchronized quiescence: severs the subscriber list under the
                                         // same lock discipline as subscribe/raise
end SharedEventSource;
