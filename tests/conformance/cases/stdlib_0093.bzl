using var guard := mutex.Acquire();
// critical section; guard disposed at scope end -> mutex released.
