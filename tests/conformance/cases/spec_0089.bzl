_ := ComputeResult();             // DISCARD: legal only when ComputeResult's return type
                                  //   is value-typed, runtime-managed, or trivial-dispose.
var _ := ComputeResult();         // BINDING: declares a local variable named "_" and assigns.
                                  //   The local is subject to normal unused-variable
                                  //   warnings AND the standard ownership rules — if the
                                  //   value needs disposal, this form does not satisfy that.
using var _ := ComputeResult();   // BINDING with disposal at scope end. Works for any
                                  //   class type, including non-trivial-dispose.
