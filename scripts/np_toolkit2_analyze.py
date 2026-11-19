"""Static analysis of Media/Plugins/UnityNpToolkit2.prx: classifies every libSceNpToolkit2 import by how the plugin
uses it and writes src/np_toolkit2_table.inc (bindings for runtime_np_toolkit2.c) plus a readable import report.
Needs capstone (pip install capstone) and the shadPS4 aerolib.inl for symbol names.

    python scripts/np_toolkit2_analyze.py --prx game/prepared/CUSA57767-1.00/Media/Plugins/UnityNpToolkit2.prx         --aerolib <shadPS4>/scripts/aerolib.inl --inc src/np_toolkit2_table.inc --report docs/np-toolkit2-imports.md

Nothing is executed; the plugin is only read.
"""
import bisect
import collections
import json
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capstone as C  # noqa: E402
import ps4_module as pm  # noqa: E402
# ---- npa ----
class Prx:
    def __init__(s,path):
        s.m=pm.read_module(Path(path))
        m=s.m
        s.loads=[p for p in m['ph'] if p['type']==1]
        top=max(p['vaddr']+p['memsz'] for p in s.loads)
        s.img=bytearray(top)
        for p in s.loads:
            s.img[p['vaddr']:p['vaddr']+p['filesz']]=m['elf'][p['offset']:p['offset']+p['filesz']]
        s.syms=m['symbols']
        s.slots={}
        s.rel={}
        for t,typ,symi,add in m['relocs']:
            if symi and symi<len(s.syms) and s.syms[symi]['section']==0 and s.syms[symi]['identity']:
                n,lib,mod=s.syms[symi]['identity']
                s.slots[t]=(f"{n}#{lib[0]}")
            else: s.rel[t]=(typ,symi,add)
        s.md=C.Cs(C.CS_ARCH_X86,C.CS_MODE_64)
        s.names={}
        for l in open('src/import_names.inc',errors='replace'):
            mm=re.match(r'\{"([^"]+)","([^"]+)"\}',l.strip())
            if mm: s.names[mm.group(1)]=mm.group(2)
        s.exports={x['name']:x['value'] for x in s.syms if x['section']!=0 and x['name']}
    def rd(s,a,n): return bytes(s.img[a:a+n])
    def stub(s,a):
        d=s.rd(a,6)
        if d[:2]==b'\xff\x25':
            slot=a+6+struct.unpack("<i",d[2:])[0]
            return s.slots.get(slot)
    def label(s,nid): return s.names.get(nid,nid)
    def dis(s,a,n=400,cnt=80,stop_ret=False):
        out=[]
        for i in s.md.disasm(s.rd(a,n),a):
            extra=''
            if i.mnemonic in('call','jmp') and i.op_str.startswith('0x'):
                t=int(i.op_str,16); st=s.stub(t)
                if st: extra='   ; IMPORT '+s.label(st)
                else: extra='   ; fn_%x'%t
            elif i.mnemonic in('mov','lea','call','jmp','cmp') and 'rip' in i.op_str:
                mm=re.search(r"rip ([+-]) (0x[0-9a-f]+)",i.op_str)
                if mm:
                    t=i.address+i.size+int(mm.group(2),16)*(1 if mm.group(1)=='+' else -1)
                    extra='   ; ->%#x'%t
                    if t in s.slots: extra+=' '+s.label(s.slots[t])
            out.append(f"{i.address:#x}: {i.mnemonic} {i.op_str}{extra}")
            cnt-=1
            if cnt<=0: break
            if stop_ret and i.mnemonic=='ret': break
        return out
if False:
    p=Prx(sys.argv[1])
    print({k:hex(v) for k,v in p.exports.items()} if len(p.exports)<60 else len(p.exports))

# ---- cfg ----
def tr(p): return [(q['vaddr'],q['vaddr']+q['filesz']) for q in p.loads if q['flags']&1]
JCC=set('jo jno jb jae je jne jbe ja js jns jp jnp jl jge jle jg jrcxz jecxz loop'.split())
def analyze(p,roots,follow_lea=False,maxfn=5000):
    T=tr(p); intext=lambda a:any(lo<=a<hi for lo,hi in T)
    funcs={}; imps={}; work=list(roots); lea_targets=set(); calls={}
    while work and len(funcs)<maxfn:
        f=work.pop()
        if f in funcs or not intext(f): continue
        if p.stub(f):
            st=p.stub(f); imps.setdefault(st,set()).add(f); funcs[f]=('imp',st); continue
        funcs[f]='fn'
        blocks=[f]; seenb=set(); calls[f]=set()
        while blocks:
            b=blocks.pop()
            if b in seenb or not intext(b): continue
            a=b
            while True:
                if a in seenb: break
                seenb.add(a)
                ins=list(p.md.disasm(p.rd(a,16),a,1))
                if not ins: break
                i=ins[0]; a=i.address+i.size
                m=i.mnemonic
                if m=='call' and i.op_str.startswith('0x'):
                    t=int(i.op_str,16); calls[f].add(t)
                    st=p.stub(t)
                    if st: imps.setdefault(st,set()).add(i.address)
                    else: work.append(t)
                elif m=='jmp':
                    if i.op_str.startswith('0x'):
                        t=int(i.op_str,16)
                        st=p.stub(t)
                        if st: imps.setdefault(st,set()).add(i.address)
                        else:
                            blocks.append(t); work.append(t)
                    break
                elif m in JCC:
                    blocks.append(int(i.op_str,16))
                elif m in('ret','ud2','hlt'): break
                elif m=='lea' and follow_lea and 'rip' in i.op_str:
                    mm=re.search(r"rip ([+-]) (0x[0-9a-f]+)",i.op_str)
                    t=i.address+i.size+int(mm.group(2),16)*(1 if mm.group(1)=='+' else -1)
                    if intext(t): lea_targets.add(t); work.append(t)
    return funcs,imps,calls

# ---- classify ----
REG64=['rax','rbx','rcx','rdx','rsi','rdi','rbp','rsp','r8','r9','r10','r11','r12','r13','r14','r15']
def r64(n):
    m={'eax':'rax','ebx':'rbx','ecx':'rcx','edx':'rdx','esi':'rsi','edi':'rdi','al':'rax','ax':'rax','bl':'rbx','cl':'rcx','dl':'rdx','sil':'rsi','dil':'rdi'}
    if n in m:return m[n]
    if re.match(r'r\d+[dwb]$',n):return n[:-1]
    return n
ARGS=['rdi','rsi','rdx','rcx','r8','r9']
class A:
    def __init__(s,p):
        s.p=p; s.ins=[]
        for lo,hi in tr(p): s.ins+=list(p.md.disasm(p.rd(lo,hi-lo),lo))
        s.idx={i.address:k for k,i in enumerate(s.ins)}
        s.starts=[]
        for k in range(1,len(s.ins)-1):
            i=s.ins[k]
            if i.mnemonic=='push' and i.op_str=='rbp' and s.ins[k+1].mnemonic=='mov' and s.ins[k+1].op_str=='rbp, rsp' and s.ins[k-1].mnemonic in('nop','int3','ret','jmp','ud2'):
                s.starts.append(k)
    def func_range(s,k):
        j=bisect.bisect_right(s.starts,k)-1
        a=s.starts[j] if j>=0 else max(0,k-80)
        b=s.starts[j+1] if j+1<len(s.starts) else len(s.ins)
        return a,b
    def stub(s,i):
        if i.mnemonic in('call','jmp') and i.op_str.startswith('0x'): return s.p.stub(int(i.op_str,16))
def ret_used(a,k):
    """is rax/eax/al read after call at index k before being overwritten"""
    for i in a.ins[k+1:k+10]:
        m=i.mnemonic; ops=[o.strip() for o in i.op_str.split(',')] if i.op_str else []
        if m in('call','ret','jmp') :
            return m=='ret' and False
        txt=i.op_str
        def reads(t): return re.search(r'\b(rax|eax|ax|al)\b',t) is not None
        if m in('mov','lea','movzx','movsx','movsxd','vmovups','movaps','movups'):
            if len(ops)==2:
                if reads(ops[1]): return True
                if re.search(r'\[.*\b(rax|eax)\b.*\]',ops[0]): return True
                if r64(ops[0])=='rax': return False
        elif m=='xor' and len(ops)==2 and ops[0]==ops[1] and r64(ops[0])=='rax': return False
        elif m.startswith('j'): continue
        else:
            if reads(txt): return True
    return False
def analyze_site(a,k,nid):
    p=a.p
    f0,f1=a.func_range(k)
    reg={}; hist={}; pend_size=None; stackrefs=set()
    for q in a.ins[f0:k]:
        m=q.mnemonic; ops=[o.strip() for o in q.op_str.split(',')] if q.op_str else []
        for mm in re.finditer(r'rbp - (0x[0-9a-f]+)',q.op_str): stackrefs.add(int(mm.group(1),16))
        if m=='call':
            st=a.stub(q)
            if st and p.label(st) in('_Znwm','_Znam'):
                reg['rax']=('new',reg.get('__sz',('imm',0))[1],q.address)
            else:
                if st and 'Toolkit2' in st:
                    o=reg.get('rdi')
                    if o and o[0] in('new','stack'): hist.setdefault(o,[]).append(st)
                reg['rax']=('ret',)
            for r in('rcx','rdx','rsi','rdi','r8','r9','r10','r11'): reg.pop(r,None)
        elif m=='mov' and len(ops)==2:
            d,s_=r64(ops[0]),r64(ops[1])
            if d in REG64 and not ops[0].startswith('['):
                if re.match(r'^(0x[0-9a-f]+|\d+)$',ops[1]):
                    v=int(ops[1],16) if ops[1].startswith('0x') else int(ops[1])
                    reg[d]=('imm',v)
                    if d=='rdi': reg['__sz']=('imm',v)
                elif s_ in reg and not ops[1].startswith('['): reg[d]=reg[s_]
                else: reg.pop(d,None)
        elif m=='lea' and len(ops)==2:
            d=r64(ops[0]); mm=re.match(r'\[rbp - (0x[0-9a-f]+)\]$',ops[1])
            if mm and d in REG64: reg[d]=('stack',int(mm.group(1),16))
            else: reg.pop(d,None)
        else:
            if ops and not m.startswith('j') and m not in('cmp','test','push'):
                d=r64(ops[0])
                if d in reg and not ops[0].startswith('['): reg.pop(d,None)
    # args
    nargs=0
    for r in ARGS:
        if r in reg: nargs+=1
        else: break
    return dict(reg={r:reg.get(r) for r in ARGS[:4]},hist=hist,stack=stackrefs,f=(f0,f1))
def stride_after(a,k):
    for i in a.ins[k+1:k+6]:
        mm=re.match(r'(add r\d+|add rsi|add rdi|add rbx), (0x[0-9a-f]+|\d+)$',i.mnemonic+' '+i.op_str)
        if mm and not mm.group(2).startswith('-'): return int(mm.group(2),16) if mm.group(2).startswith('0x') else int(mm.group(2))
    return None
def classify(path):
    p=Prx(path); a=A(p)
    sites=collections.defaultdict(list)
    for k,i in enumerate(a.ins):
        st=a.stub(i)
        if st and 'Toolkit2' in st: sites[st].append(k)
    out={}
    for nid,ks in sites.items():
        ev=[]
        for k in ks:
            info=analyze_site(a,k,nid)
            reg=info['reg']; o1,o2=reg['rdi'],reg['rsi']
            used=ret_used(a,k)
            ev.append(dict(k=k,addr=a.ins[k].address,o1=o1,o2=o2,hist=info['hist'],used=used,stack=info['stack'],
                           stride=stride_after(a,k),nargs=sum(1 for r in ARGS[:4] if reg[r])))
        out[nid]=ev
    return p,a,out

# ---- aggr ----
def fullstack(a,f):
    s=set()
    for q in a.ins[f[0]:f[1]]:
        for mm in re.finditer(r'rbp - (0x[0-9a-f]+)',q.op_str): s.add(int(mm.group(1),16))
    return s
def aggregate(p,a,out):
    res={}
    for nid,ev in out.items():
        n=nid.split('#')[0]
        r=dict(sites=len(ev),submit=0,ctor=0,sizes=[],used=sum(e['used'] for e in ev),nargs=collections.Counter(e['nargs'] for e in ev),roles=set())
        for e in ev:
            o1,o2=e['o1'],e['o2']
            f=a.func_range(e['k'])
            if o1 and o2 and o1[0] in('new','stack') and o2[0] in('new','stack') and o1!=o2 and e['used'] and e['nargs']>=2:
                r['submit']+=1
            pre_written=False
            if o1 and o1[0]=='stack':
                fs=fullstack(a,f); above=[z for z in fs if z<o1[1]]
                span=o1[1]-max(above) if above else 0
                for q in a.ins[f[0]:e['k']]:
                    mm=re.match(r'^\w+ ptr \[rbp - (0x[0-9a-f]+)\],',q.op_str)
                    if mm and q.mnemonic.startswith(('mov','vmov','xor','and','or')):
                        K=int(mm.group(1),16)
                        if o1[1]-span<K<=o1[1]: pre_written=True
            if o1 and o1[0] in('new','stack') and not pre_written and not e['hist'].get(o1) and e['nargs']==1 and not e['used']:
                r['ctor']+=1
                if o1[0]=='new' and o1[1]: r['sizes'].append(o1[1])
                elif o1[0]=='stack':
                    fs=fullstack(a,f); above=[z for z in fs if z<o1[1]]
                    r['sizes'].append(('gap',o1[1]-max(above)) if above else ('gap',0))
                if e['stride']: r['sizes'].append(('stride',e['stride']))
        res[n]=r
    return res

# ---- final ----
NAMED={'vbh-MWtjMh4':'INIT_PARAMS_CTOR','-bAjEe-hdCg':'INIT_PARAMS_DTOR','s0u30pOfi9Q':'TERM_PARAMS_CTOR','vycUuYWwilI':'TERM_PARAMS_DTOR','LLdnqVnnNBQ':'CORE_INIT','IcuECtP3L0E':'CORE_TERM'}
BASE={'mIinAn8TvdY':'BUSY','El4tFb9f8dk':'FUNC','5-UHpg8s00I':'RC'}
def ret_kind(a,k):
    regs={'rax','eax','al','ax'}
    alias=set(['rax'])
    kinds=[]
    for i in a.ins[k+1:k+12]:
        m=i.mnemonic; ops=[o.strip() for o in i.op_str.split(',')] if i.op_str else []
        if m in('call','ret','jmp'): break
        txt=i.op_str
        mem=re.findall(r'\[([^\]]*)\]',txt)
        for mm in mem:
            if any(re.search(r'\b%s\b'%x,mm) for x in alias): kinds.append('ptr')
        if m=='test' and len(ops)==2 and ops[0]==ops[1]:
            if ops[0] in('rax',): kinds.append('ptr')
            elif ops[0] in('eax','al'): kinds.append('int')
        if m=='cmp' and ops and ops[0] in('eax','al'): kinds.append('int')
        if m=='mov' and len(ops)==2:
            if ops[1]=='rax' and ops[0] not in('rax',) and re.match(r'r',ops[0]): alias.add(ops[0]); kinds.append('ptr?')
            if ops[1]=='eax' : kinds.append('int')
            if ops[1] in('rax',) and ops[0].startswith('['): kinds.append('ptr?')
        if m in('movzx','setne','sete','setns') : kinds.append('int')
        if m=='mov' and len(ops)==2 and ops[0]=='rdi' and ops[1] in alias: kinds.append('ptr')
        if m=='mov' and len(ops)==2 and ops[0]=='rsi' and ops[1] in alias: kinds.append('ptr')
    if 'ptr' in kinds: return 'ptr'
    if 'int' in kinds: return 'int'
    if 'ptr?' in kinds: return 'ptr'
    return None
def run(path,startup_nids):
    p,a,out=classify(path)
    res=aggregate(p,a,out)
    final={}
    for nid,ev in out.items():
        n=nid.split('#')[0]; r=res[n]
        if n in NAMED: kind=('NAMED',NAMED[n],0)
        elif n in BASE: kind=('BASE',BASE[n],0)
        elif r['submit']: kind=('SUBMIT','',0)
        elif r['ctor'] or any(e['stride'] for e in ev):
            sz=[x if isinstance(x,int) else x[1] for x in r['sizes'] if (isinstance(x,int) or x[1]>0)]
            news=[x for x in r['sizes'] if isinstance(x,int)]+[e['stride'] for e in ev if e['stride']]
            size=min(news) if news else (min(sz) if sz else 0)
            kind=('CTOR','',size)
        else:
            rk=[ret_kind(a,e['k']) for e in ev]
            used=[x for x in rk if x]
            if used and len(used)!=len(rk): kind=('MIXED','',0)
            elif used:
                kind=('GET','ptr' if 'ptr' in used else 'int',0)
            else: kind=('VOID','',0)
        final[n]=dict(kind=kind,sites=len(ev),addrs=[hex(e['addr']) for e in ev][:4])
    return p,a,final
if False:
    p,a,final=run(sys.argv[1],None)
    c=collections.Counter(v['kind'][0]+':'+v['kind'][1] for v in final.values())
    print(c)
    json.dump(final,open(sys.argv[2],'w'),indent=0)

# ---- symbols, naming and output ----
OFFLINE_NOTE = 'fail-closed offline'
CORE_NAMED = {
    'vbh-MWtjMh4': 'INIT_PARAMS_CTOR', '-bAjEe-hdCg': 'INIT_PARAMS_DTOR', 's0u30pOfi9Q': 'TERM_PARAMS_CTOR',
    'vycUuYWwilI': 'TERM_PARAMS_DTOR', 'LLdnqVnnNBQ': 'CORE_INIT', 'IcuECtP3L0E': 'CORE_TERM',
}
LIB = 'libSceNpToolkit2'


def load_aerolib(path):
    names = {}
    for line in Path(path).read_text(errors='replace').splitlines():
        m = re.search(r'STUB\(\s*"([^"]+)"\s*,\s*([^)\s]+)\s*\)', line)
        if m:
            names[m.group(1)] = m.group(2)
    return names


def refine(final, names):
    """Name-based corrections; a disagreement with the code analysis is reported, not hidden."""
    notes = []
    for nid, entry in final.items():
        sym = names.get(nid)
        if not sym or nid in CORE_NAMED:
            continue
        kind = entry['kind'][0]
        new = None
        if re.search(r'RequestBaseC2E', sym):
            new = ('REQBASE', '', 0)
        elif re.search(r'D[012]E', sym):
            new = ('VOID', '', 0)
        elif re.search(r'C[12]E', sym):
            new = entry['kind'] if kind == 'CTOR' else ('CTOR', '', 0)
        elif sym.endswith('3getEv'):
            new = ('GET', 'ptr', 0)
        elif 'getReturnCode' in sym:
            new = ('BASE', 'RC', 0)
        elif 'isLocked' in sym:
            new = ('BASE', 'BUSY', 0)
        elif 'getFunctionType' in sym:
            new = ('BASE', 'FUNC', 0)
        elif 'getServiceType' in sym:
            new = ('BASE', 'SVC', 0)
        elif 'abortRequest' in sym:
            new = ('GET', 'int', 0)
        elif 'getServerError' in sym:
            new = ('GET', 'ptr', 0)
        elif re.search(r'(set|aS)\w*E', sym) and kind in ('VOID', 'GET', 'MIXED'):
            new = ('VOID', '', 0)
        if new and new != entry['kind']:
            notes.append((nid, sym, entry['kind'], new))
            entry['kind'] = new
    return notes


def request_ctor(out, final, nid):
    """NID of the request-constructor that built the first argument of a submit call."""
    for e in out.get(nid + '#' + LIB, []):
        o1 = e['o1']
        if o1 and e['hist'].get(o1):
            return e['hist'][o1][0].split('#')[0]
    return None


def name_submits(final, names, out):
    ctor_names = {}
    for nid in final:
        if final[nid]['kind'][0] == 'SUBMIT':
            ctor = request_ctor(out, final, nid)
            if ctor:
                ctor_names[nid] = ctor
    gets = []
    for sym in names.values():
        m = re.match(r'_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_(\d+)([A-Za-z0-9]+)(\d+)([A-Za-z0-9]+)EE3getEv', sym)
        if m:
            gets.append((m.group(2), m.group(4)))
    prefix = '_ZN3sce7Toolkit2NP2V2'
    found = {}
    for nid, ctor in ctor_names.items():
        sym = names.get(ctor, '')
        m = re.match(r'_ZN3sce7Toolkit2NP2V2(\d+)([A-Za-z0-9]+)7Request(\d+)([A-Za-z0-9]+)C1Ev$', sym)
        if not m:
            continue
        ns, cls = m.group(2), m.group(4)
        funcs = {cls[0].lower() + cls[1:]}
        acronym = re.match(r'^([A-Z]+)([A-Z][a-z].*)$', cls)
        if acronym:
            funcs.add(acronym.group(1).lower() + acronym.group(2))
        for func in funcs:
            head = f'{prefix}{len(ns)}{ns}{len(func)}{func}ERKNS3_7Request{len(cls)}{cls}E'
            tails = [f'PNS2_4Core8ResponseINS{s}_5EmptyEEE' for s in range(3, 20)]
            for tn, tc in gets:
                for s in (3, 4):
                    tails.append(f'PNS2_4Core8ResponseINS{s}_{len(tc)}{tc}EEE')
                    tails.append(f'PNS2_4Core8ResponseINS{s}_{len(tn)}{tn}{len(tc)}{tc}EEE')
            for tail in tails:
                if pm.nid(head + tail) == nid:
                    found[nid] = head + tail
    return found, ctor_names


def namespace(sym):
    m = re.match(r'_ZNK?3sce7Toolkit2NP2V2(\d+)(\w+)', sym or '')
    if not m:
        return None
    n = int(m.group(1))
    return m.group(2)[:n]


DOMAINS = {
    'Core': 'CORE', 'UserProfile': 'USER', 'Auth': 'USER', 'Trophy': 'TROPHY', 'Presence': 'PRESENCE',
    'Friend': 'FRIENDS', 'Matching': 'MATCHING', 'Session': 'SESSION', 'TUS': 'TUS', 'TSS': 'TUS',
    'Commerce': 'COMMERCE', 'Wordfilter': 'WORD_FILTER', 'NetworkUtils': 'NETWORK_UTILS',
}


def domain(sym, kind):
    ns = namespace(sym)
    inner = re.search(r'Core8ResponseINS\d+_(\d+)', sym or '')
    if ns == 'Core' and inner:
        start = inner.end()
        word = re.match(r'[A-Za-z]+', sym[start:])
        ns = word.group(0)[:int(inner.group(1))] if word else ns
        if ns == 'Empty':
            return 'STRUCTURAL'
    if ns == 'Core':
        return 'STRUCTURAL' if kind in ('CTOR', 'VOID', 'BASE', 'REQBASE') else 'CORE'
    return DOMAINS.get(ns, 'OTHER')


def startup_nids(p):
    inv = {k.split('#')[0]: v for k, v in p.exports.items()}
    nids = set()
    for name in ('PrxInitialize', 'PrxUpdate'):
        funcs, imps, _ = analyze(p, [inv[pm.nid(name)]])
        nids |= {k.split('#')[0] for k in imps if LIB in k}
    return nids


INC_HEADER = ('/* Generated by scripts/np_toolkit2_analyze.py from Media/Plugins/UnityNpToolkit2.prx; edit the script, not this file.\n'
              ' * TK2_<kind>(index, nid [, size], symbol): the behaviour is defined in runtime_np_toolkit2.c. */\n')


def emit(final, names, submit_syms, startup, unnamed_ctor_ns, path):
    lines = [INC_HEADER]
    index = 0
    for nid in sorted(final):
        kind = final[nid]['kind']
        sym = names.get(nid) or submit_syms.get(nid) or ''
        if nid in CORE_NAMED:
            lines.append(f'TK2_NAMED("{nid}","{sym}")\n')
            continue
        k, sub, size = kind
        if k == 'CTOR':
            lines.append(f'TK2_CTOR({index},"{nid}",{size},"{sym}")\n')
        elif k == 'VOID':
            lines.append(f'TK2_VOID({index},"{nid}","{sym}")\n')
        elif k == 'SUBMIT':
            lines.append(f'TK2_SUBMIT({index},"{nid}","{sym}")\n')
        elif k == 'GET':
            lines.append(f'TK2_GET{"P" if sub == "ptr" else "I"}({index},"{nid}","{sym}")\n')
        elif k == 'BASE':
            lines.append(f'TK2_{sub}({index},"{nid}","{sym}")\n')
        elif k == 'REQBASE':
            lines.append(f'TK2_REQBASE({index},"{nid}","{sym}")\n')
        else:
            lines.append(f'TK2_UNBOUND("{nid}","{sym}")\n')
        index += 1
    for nid in sorted(startup):
        lines.append(f'TK2_STARTUP("{nid}")\n')
    Path(path).write_text(''.join(lines), newline='\n')


def report(final, names, submit_syms, startup, path, build_note):
    rows = []
    for nid in sorted(final):
        sym = names.get(nid) or submit_syms.get(nid) or ''
        k = final[nid]['kind']
        dom = domain(sym, k[0]) if sym else 'OTHER'
        label = k[1] if k[0] in ('NAMED', 'BASE', 'GET') else k[0]
        rows.append((nid, sym or '(unnamed)', dom, k[0] + (':' + k[1] if k[1] else '') + (f' size {k[2]:#x}' if k[2] else ''),
                     'startup' if nid in startup else 'later', final[nid]['sites'], ', '.join(final[nid]['addrs'][:3])))
    out = ['# UnityNpToolkit2 libSceNpToolkit2 imports\n\n', build_note + '\n\n',
           '| NID | Symbol | Class | Binding | Reach | Sites | First call sites |\n', '|---|---|---|---|---|---|---|\n']
    for r in rows:
        out.append('| ' + ' | '.join(str(x).replace('|', '\\|') for x in r) + ' |\n')
    Path(path).write_text(''.join(out), newline='\n')


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--prx', required=True, type=Path)
    ap.add_argument('--aerolib', required=True, type=Path)
    ap.add_argument('--inc', type=Path)
    ap.add_argument('--report', type=Path)
    ap.add_argument('--json', type=Path)
    args = ap.parse_args()
    names = load_aerolib(args.aerolib)
    p, a, final = run(str(args.prx), None)
    _, _, out = classify(str(args.prx))
    for slot_nid in {v.split('#')[0] for v in p.slots.values() if LIB in v}:
        final.setdefault(slot_nid, {'kind': ('UNBOUND', '', 0), 'sites': 0, 'addrs': []})
    notes = refine(final, names)
    submit_syms, _ = name_submits(final, names, out)
    startup = startup_nids(p)
    for n in notes:
        print('refined', n[0], n[1], n[2], '->', n[3])
    counts = collections.Counter(v['kind'][0] for v in final.values())
    print(dict(counts), 'named', sum(1 for n in final if n in names), 'submit names', len(submit_syms))
    if args.inc:
        emit(final, names, submit_syms, startup, None, args.inc)
    if args.report:
        report(final, names, submit_syms, startup, args.report, f'Source: `{args.prx.name}` sha256 `{p.m["sha256"]}`.')
    if args.json:
        args.json.write_text(json.dumps({'kinds': {k: v['kind'] for k, v in final.items()}, 'startup': sorted(startup)}, indent=0))


if __name__ == '__main__':
    main()
