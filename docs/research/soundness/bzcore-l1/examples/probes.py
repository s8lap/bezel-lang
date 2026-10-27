"""Hand-written adversarial probes (README §7): verdict, per-path outcomes, trace, and the
free-in-full oracle under the #4602 conditions as worded vs. with every condition."""
import importlib.util, os
spec = importlib.util.spec_from_file_location('bz', os.path.join(os.path.dirname(__file__), '..', 'bzcore.py'))
bz = importlib.util.module_from_spec(spec); spec.loader.exec_module(bz)
P = [
 ('A overwrite of a disposed binding under a live borrow', [('new','a'),('brw','b','a'),('disp','a'),('asg','a'),('use','b')]),
 ('B slot-displaced value disposed while a borrow of the slot lives', [('new','a'),('fbrw','b','a'),('fmove','q','a'),('disp','q'),('use','b')]),
 ('C disposed then thrown; the arm reads it', [('new','a'),('try',[('disp','a'),('throw','a')],'e',[('use','e')])]),
 ('D loop: borrow, kill, use, reassign', [('new','a'),('while',[('brw','b','a'),('disp','a'),('use','b'),('asg','a')])]),
 ('E nested try: inner arm throws fresh; outer arm reads the moved binding', [('new','a'),('try',[('try',[('throw','a')],'e',[('tthrow',)])],'e',[('use','a')])]),
 ('F explicit Dispose of a using var', [('using','a'),('disp','a')]),
 ('G borrow-pin keeps the borrow; Dispose under it faults later', [('new','a'),('brw','b','a'),('usingb','p','b'),('disp','a'),('use','p')]),
 ('H uninit on one arm', [('decl','a'),('if',[('asg','a')],[]),('use','a')]),
 ('I throw arm absorbs (K7.5)', [('new','a'),('if',[('throw','a')],[]),('use','a')]),
 ('J store back the displaced value then dispose the holder', [('new','a'),('fmove','q','a'),('fstm','a','q'),('disp','a')]),
 ('K slot overwrite order (K9.5)', [('new','a'),('fst','a')]),
 ('L send a holder whose slot is borrowed', [('new','a'),('fbrw','b','a'),('send','a'),('use','b')]),
 ('M move the holder; borrow of the slot survives through the new holder', [('new','a'),('fbrw','b','a'),('move','c','a'),('use','b')]),
 ('N dispose in handler of an outer binding; use after try', [('new','a'),('try',[('tthrow',)],'e',[('disp','a')]),('use','a')]),
 ('O catch binding re-borrowed into an outer binding (K2.3)', [('new','a'),('decl','c'),('try',[('throw','a')],'e',[('asgb','c','e')]),('use','c')]),
 ('P two kills on both arms then use through borrow', [('new','a'),('brw','b','a'),('if',[('disp','a')],[('disp','a')]),('use','b')]),
 ('Q F1: slot-displaced value sent while a borrow of the slot lives (K4.3 residual)', [('new','a'),('fbrw','b','a'),('fmove','c','a'),('send','c'),('use','b')]),
 ('R conditional kill then unwind (condition 2 reader)', [('new','a'),('if',[],[('disp','a')]),('tthrow',)]),
]
for title, prog in P:
    v, w, ctx = bz.check(prog)
    line = f'{title}\n    {bz.oneline(prog)}\n    verdict={v} warnings={w}'
    if v == 'accept':
        runs = bz.run_all(prog, ctx)
        line += f' bad={sorted(set(bz.judge(runs)))}\n    runs={[r.outcome for r in runs]}'
        for r in runs[:2]:
            line += f'\n    trace={[e for e in r.events if e[0] in ("new","disp","noop","release")]}'
        for name, conds in (('ruling-as-worded', ('borrow','later')), ('all-conditions', ('borrow','self','slot','later'))):
            fs = bz.free_sites(ctx, conds)
            if fs:
                rr = bz.run_all(prog, ctx, mode='free', free_sites=fs)
                line += f'\n    free[{name}] sites={len(fs)} runs={[r.outcome for r in rr]}'
    print(line)
