using var primary := new Shared<Connection>(Connect("db"));   // consumes the owned Connection; count = 1
using var second  := primary.Clone();                          // count = 2; an independent owned handle
second.Get().Ping();                                           // borrow of the underlying Connection
// scope end: second disposed (count 1), then primary disposed (count 0) → Connection disposed
