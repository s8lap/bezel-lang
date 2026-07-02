// Representative forms; the stdlib section declares the full set.
Domain.Spawn(entry: () -> ()): owned Task
Domain.Spawn<R>(entry: () -> owned R): owned OwningTask<R>            // R: sendable class
Domain.Spawn<A, R>(entry: (owned A) -> owned R, owned arg: A): owned OwningTask<R>
Domain.Spawn<R>(entry: () -> R): owned Task<R>                       // R: sendable value type
