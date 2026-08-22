using var token := await sem.AcquireAsync();
DoCriticalWork();
// token disposed at scope end -> semaphore released.
