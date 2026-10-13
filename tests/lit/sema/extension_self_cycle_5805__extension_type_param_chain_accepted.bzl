// RUN: %bezelc -c -o %t.o %s
//
// spec §14.2, v0.196 / v0.338 (spec :5672) / #5805 follow-up: the over-rejection
// guard for the extension self/cycle rule. A non-cyclic chain of type-parameter
// bounds (`where T: U where U: Exception`) is admitted; only a self-bound or a
// cycle is rejected. (Same shape as the chain accepted in
// tests/sema/extension_unconstrained_type_param_3083_test.cpp.)

module M;

class C<T, U> end C;

extension C<T, U> where T: U where U: Exception
  public method Foo(): int => 1;
end;
