// fix #3079 (stdlib §2.1 / §6.9 / language spec §3-#605, v0.249): the
// BUILT-IN-receiver half of the new `Console.Write(Line)` argument guard —
// #2935's ruling applied at the third value-demanding site.
//
// stdlib §6.9's `ChannelSender<T>.Send(owned item: T)` declares no return type,
// so it yields nothing; sema does not type the call at all (§3/#605 permissive
// acceptance), and codegen's arm emits a real `bz_channel_sender_send` and then
// returns nullptr. Before this fix `Console.WriteLine(sender.Send(42));`
// compiled clean and the whole statement vanished — the send happened, the line
// did not, and nothing said so.
//
// #2966's position-independent gate cannot see this one: it requires that NO
// dispatch arm emitted any code, and this arm emits the send. The value demand
// is what closes it here, exactly as at a local declaration (#2935) and at
// §21.7's `return expr;` — stdlib §2.1 declares `WriteLine(value: string)` /
// `WriteLine(value: Object)` / the per-type primitive overloads and NO
// value-less one-argument form, so an argument that produced no value cannot be
// rendered. The predicate is the SHARED `builtinReceiverOfMemberCall`, not a
// fourth copy of it (#2924's four-way-mirror finding), so a built-in added to
// the registry later is covered here without an edit.
//
// REJECT: `%not` is required — lit drops a non-zero-exit producer's output in a
// pipe.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module N3079LitB;
class C
  public method Push(ch: Channel<int>): int
    using var sender: ChannelSender<int> := ch.NewSender();
    Console.WriteLine(sender.Send(42));
    return 0;
  end Push;
  public static method Main(): int
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Send` on built-in type `ChannelSender` produced no value, so it cannot supply this `Console.WriteLine` argument.
