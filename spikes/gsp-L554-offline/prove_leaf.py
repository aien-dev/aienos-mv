#!/usr/bin/env python3
"""Bounded symbolic lift of the actual 44-byte RV64 leaf, not a compiler proof.
Capstone decodes bytes. Only explicitly modeled instructions are accepted.
Z3 compares every returning path with an independent 65-bit endpoint contract.
"""
from pathlib import Path
import hashlib,json,sys
import capstone,z3
from capstone.riscv import RISCV_OP_REG,RISCV_OP_IMM

MASK=(1<<64)-1
CODE_SHA='a5d388a59f99f2e5afe44f3dab0b7eb3ad8832240e17ece7e4bc4555728996f5'

def lift(code):
    md=capstone.Cs(capstone.CS_ARCH_RISCV,capstone.CS_MODE_RISCV64|capstone.CS_MODE_RISCVC);md.detail=True
    ins=list(md.disasm(code,0x28));assert sum(x.size for x in ins)==len(code)
    table={x.address:x for x in ins};args=z3.BitVecs('a n b m',64)
    states=[(0x28,dict(zip(['a0','a1','a2','a3'],args)),z3.BoolVal(True),[])];returns=[]
    while states:
        pc,regs,path,trace=states.pop();assert len(trace)<64 and len(states)<128
        assert pc in table,hex(pc);i=table[pc];nxt=pc+i.size;trace=trace+[pc]
        operands=[i.reg_name(o.reg) if o.type==RISCV_OP_REG else o.imm if o.type==RISCV_OP_IMM else None for o in i.operands]
        def value(name):
            if name=='zero':return z3.BitVecVal(0,64)
            assert name in regs,(hex(pc),name,'read of unspecified register')
            return regs[name]
        op=i.mnemonic
        if op in ('c.mv','mv'):regs={**regs,operands[0]:value(operands[1])}
        elif op in ('c.li','li'):regs={**regs,operands[0]:z3.BitVecVal(operands[1],64)}
        elif op=='add':regs={**regs,operands[0]:value(operands[1])+value(operands[2])}
        elif op=='sltu':regs={**regs,operands[0]:z3.If(z3.ULT(value(operands[1]),value(operands[2])),z3.BitVecVal(1,64),z3.BitVecVal(0,64))}
        elif op in ('c.beqz','beqz','bltu','bgeu'):
            if op in ('c.beqz','beqz'):cond=value(operands[0])==0;delta=operands[1]
            else:
                cond=z3.ULT(value(operands[0]),value(operands[1]))
                if op=='bgeu':cond=z3.Not(cond)
                delta=operands[2]
            target=pc+delta;assert target in table
            states.append((target,dict(regs),z3.And(path,cond),trace))
            states.append((nxt,dict(regs),z3.And(path,z3.Not(cond)),trace));continue
        elif (op=='c.jr' and operands==['ra']) or op=='ret':
            returns.append((path,value('a0'),trace));continue
        elif op in ('nop','c.nop'):pass
        else:raise AssertionError((hex(pc),op,i.op_str,'unmodeled instruction'))
        states.append((nxt,regs,path,trace))
    return args,returns,[dict(pc=i.address,bytes=i.bytes.hex(),op=i.mnemonic,operands=i.op_str) for i in ins]

def contract(args,mode='correct'):
    a,n,b,m=[z3.ZeroExt(1,v) for v in args]
    nonempty=z3.And(n!=0,m!=0)
    overflow=z3.Or(z3.UGT(a+n,MASK),z3.UGT(b+m,MASK))
    intersect=z3.And(z3.ULT(a,b+m),z3.ULT(b,a+n))
    if mode=='no-empty-guard':nonempty=z3.BoolVal(True)
    if mode=='no-overflow':overflow=z3.BoolVal(False)
    if mode=='inclusive':intersect=z3.And(z3.ULE(a,b+m),z3.ULE(b,a+n))
    return z3.If(z3.And(nonempty,z3.Or(overflow,intersect)),z3.BitVecVal(1,64),z3.BitVecVal(0,64))

def prove(code,out):
    assert hashlib.sha256(code).hexdigest()==CODE_SHA
    args,paths,decoded=lift(code);results=[]
    for mode in ['correct','no-empty-guard','no-overflow','inclusive']:
        s=z3.Solver();s.set(timeout=60000);s.add(z3.Or(*[z3.And(p,r!=contract(args,mode)) for p,r,t in paths]))
        (out/('leaf-'+mode+'.smt2')).write_text(s.to_smt2())
        verdict=s.check();row=dict(contract=mode,verdict=str(verdict))
        if mode=='correct':assert verdict==z3.unsat,verdict
        else:
            assert verdict==z3.sat,verdict;model=s.model();row['counterexample']=[model.eval(x,model_completion=True).as_long() for x in args]
        results.append(row)
    s=z3.Solver();s.set(timeout=60000);s.add(z3.Not(z3.Or(*[p for p,r,t in paths])));covered=s.check();assert covered==z3.unsat,covered
    feasibility=[]
    for path,ret,trace in paths:
        s=z3.Solver();s.set(timeout=60000);s.add(path);v=s.check();assert v in (z3.sat,z3.unsat)
        feasibility.append(dict(trace=[hex(x) for x in trace],feasible=v==z3.sat))
    result=dict(z3=z3.get_version_string(),capstone=capstone.__version__,code_sha256=CODE_SHA,decoded=decoded,return_paths=len(paths),path_details=feasibility,coverage_counterexample=str(covered),comparisons=results,scope='All four-u64 argument combinations under the listed instruction semantics. Not a proof of the decoder, emulator, compiler, GPU hardware, or full firmware.')
    (out/'symbolic-proof.json').write_text(json.dumps(result,indent=2)+'\n');return result

if __name__=='__main__':
    snap=Path(sys.argv[1]);out=Path(sys.argv[2]);print(json.dumps(prove((snap/'fwimage.bin').read_bytes()[0x28:0x54],out),indent=2))
