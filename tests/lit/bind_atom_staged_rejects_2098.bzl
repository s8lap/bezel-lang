// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §17.15 / §30.5 / §28.3 K8.5 / K7.4, v0.216 (impl #2098) — the
// STAGED, targeted rejections of the first landing (each a clean diagnostic
// in place of a silent wrong lowering; tracked by the follow-up issue the
// diagnostic's remedy implies):
//   (1) `await` inside a binding atom in an async body — the async
//       lowering's frame-local scan is statement-level, so an atom binding
//       would stay alloca-backed across the suspension (#2122).
//   (2) a CONDITIONAL consume of an atom binding (an `owned` argument inside
//       one if-atom arm) — K7.4's per-path boundary disposal is not
//       implemented for the atom boundary.
//   (3) an owned-producer clause form outside the decidable set (here a
//       Task-family mint) — declined with reasons rather than silently
//       leaked.
module M;
class Res
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor Console.WriteLine("D"); end destructor;
end Res;
class W
  public static method Sink(owned r: Res): int
    var v := r.Id;
    r.Dispose();
    return v;
  end Sink;
end W;
class C
  public static async method Get(): Task<int>
    return 7;
  end Get;
  public static method T(): bool
    return true;
  end T;

  // (1) await-in-atom (async body).
  public static async method A(): Task<int>
    var v := (var t := await C.Get(); t + 1);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `await` inside a `(var {{.*}})` binding atom is not supported in this version
    // CHECK-SAME: (§17.15 / §30.5, v0.216)
    return v;
  end A;

  // (2) conditional consume of an atom binding.
  // §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.
  public static method B(): int
    return (var c := new Res(1); (if C.T() then W.Sink(move c) else 0));
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: this consume of binding-atom binding `c` is not supported inside the atom
    // CHECK-SAME: (§17.15 / §28.3 K8.5, v0.216)
  end B;

  // (3) an owned clause-initializer form outside the decidable set: a
  // Task-family call MINTS an owned frame (§28.3 v0.190) the atom boundary
  // cannot yet dispose.
  public static method D(): int
    return (var t := C.Get(); 0);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: this owned initializer form is not supported for a binding-atom clause (`t`)
    // CHECK-SAME: (§17.15 / §28.3 K8.5, v0.216)
  end D;
end C;
