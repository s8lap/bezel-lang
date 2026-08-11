// jobs: OwningStack<Job>
var top := jobs.Peek();          // borrow — the stack still owns it
using var next := jobs.Pop();    // transfer — yours now, disposed at scope end
