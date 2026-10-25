"""Development helper: run discovery on Game.exe and compare with Ghidra's function list."""
import sys, os, time, importlib.util
sys.path.insert(0, os.path.dirname(__file__))
from pe import PE
from x86 import Decoder
from discover import Discovery, data_code_pointers

exe = sys.argv[1]
spec = importlib.util.spec_from_file_location('cfg', sys.argv[2])
cfg = importlib.util.module_from_spec(spec); spec.loader.exec_module(cfg)
pe = PE(exe)
dec = Decoder(pe)
iat = {va: name for dll, name, va in pe.imports}
t0 = time.time()
d = Discovery(pe, dec, iat, cfg.NORETURN_IMPORTS, cfg.EXTRA_ENTRIES, cfg.JUMP_TABLES, cfg.NOT_ENTRIES)
ptrs = data_code_pointers(pe, dec)
ghidra = []
if len(sys.argv) > 3:
    ghidra = [int(l.split('\t')[0], 16) for l in open(sys.argv[3])]
funcs = d.run([pe.entry], ptrs)
print('time %.1fs' % (time.time() - t0))
print('functions', len(funcs), 'insns', sum(len(f.insns) for f in funcs.values()))
print('unique insn addrs', len(set(a for f in funcs.values() for a in f.insns)))
print('noreturn', len(d.noreturn), ' '.join('%x' % x for x in sorted(d.noreturn)))
print('jump tables', sum(len(f.jtables) for f in funcs.values()))
errs = [(e, a, m) for e, f in funcs.items() for a, m in f.errors]
print('errors', len(errs))
for e in errs[:40]: print('  %08x %08x %s' % e)
print('rejected candidates', len(d.rejected))
gs = set(ghidra)
print('ghidra not found by us', len(gs - set(funcs)), ' '.join('%x' % x for x in sorted(gs - set(funcs))[:40]))
print('ours not in ghidra', len(set(funcs) - gs))
ind = sum(len(f.indirect_jumps) for f in funcs.values())
print('indirect jumps (tail calls)', ind, [('%x' % a) for f in funcs.values() for a in f.indirect_jumps][:20])
for l in d.log[:30]: print(l)
import pickle
pickle.dump({'funcs': funcs, 'noreturn': d.noreturn, 'rejected': d.rejected}, open('work/survey.pkl', 'wb'))
