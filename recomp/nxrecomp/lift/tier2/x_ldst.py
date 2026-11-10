# SPDX-License-Identifier: GPL-3.0-or-later
"""Tier-2 handlers for one instruction family (ldst). A method named i_<mnemonic> here OVERRIDES the same-named handler of
Wonder's ported mixins (this class comes first in Tier2's base list), so a faulty ported handler is fixed by redefining it
here, and a missing one is added here. Same-named helpers for the generated C++ go in include/nxrecomp/cxx_ldst.h.
Handler signature: i_<mnemonic>(self, f, i, ops) -> one C++ statement (a string); raise Unhandled('why') when a form is
not supported. i is the Capstone instruction (i.address, i.mnemonic, i.op_str, i.operands, i.writeback), ops = i.operands."""
import re

from capstone import arm64_const as A

from .base import B, VAS, Reg, Unhandled, subst, u, s  # noqa: F401

ES_OF = {"b": 1, "h": 2, "s": 4, "d": 8}  # arrangement letter -> element bytes


class XLdst:
    def xst(self, i, ops):
        """AdvSIMD structure load/store: LDn/STn (interleaved for n>1, consecutive for LD1/ST1), the single-lane
        forms (`}[lane]`) and the replicating LDnR. Writeback amount = bytes transferred, or the register for a
        register post-index (Rm = XZR means the immediate form)."""
        m = re.fullmatch(r"(ld|st)([1-4])(r?)", i.mnemonic)
        if not m:
            raise Unhandled("mnemonic")
        store, n, repl = m.group(1) == "st", int(m.group(2)), m.group(3) == "r"
        regs_ops, mem, _ = self.mem_ops(ops)
        if mem is None or not regs_ops:
            raise Unhandled("operands")
        if mem.mem.disp or mem.mem.index:
            raise Unhandled("address form")
        rs = [self.regop(i, o) for o in regs_ops]
        if any(r.kind != "v" for r in rs):
            raise Unhandled("non-vector register")
        if store and repl:
            raise Unhandled("store replicate")
        am = re.match(r"\{v\d+\.(\d*)([bhsd])", i.op_str)
        if not am:
            raise Unhandled("arrangement")
        es = ES_OF[am.group(2)]
        cnt = int(am.group(1)) if am.group(1) else None  # elements per register (None for a lane form)
        lane = max(o.vector_index for o in regs_ops)
        if "}[" in i.op_str:
            mode = "lane"
            if lane < 0:
                raise Unhandled("lane index")
        elif repl:
            mode = "repl"
        else:
            mode = "seq" if n == 1 else "il"
        nr = len(rs)
        if mode == "seq":
            if not 1 <= nr <= 4 or cnt is None:
                raise Unhandled("register count")
            dsz = cnt * es
            total = nr * dsz
        else:
            if nr != n or cnt is None and mode != "lane":
                raise Unhandled("register count")
            dsz = (cnt or 1) * es
            total = nr * (dsz if mode == "il" else es)
        regn = [r.n for r in rs]

        base = Reg(i.reg_name(mem.mem.base))
        bn = base.n
        # Capstone drops the post-index operand for LDnR and single-lane LD3/ST3 (it keeps it for the others), so the
        # writeback is read from the text: "], #imm" or "], xm"; Rm = XZR is the immediate form.
        wb = None
        pm = re.search(r"\],\s*(#\S+|[xw](?:\d+|zr))\s*$", i.op_str)
        if pm:
            tok = pm.group(1)
            if tok.startswith("#"):
                if int(tok[1:], 0) != total:
                    raise Unhandled("post-index amount")
                wb = f"c->x[{bn}] += {total}ull;"
            elif tok in ("xzr", "wzr"):
                wb = f"c->x[{bn}] += {total}ull;"
            else:
                wb = f"c->x[{bn}] += {Reg(tok).r()};"

        p = [f"uint64_t a = c->x[{bn}];"]
        if store:
            if mode == "seq":
                for k in range(nr):
                    p.append(f"nx::vst_n<{dsz}>(a + {k * dsz}, c->v[{regn[k]}]);")
            elif mode == "il":
                p.append(f"nx::vst_il<{n}, {es}, {cnt}>(a, {{{', '.join(f'c->v[{r}]' for r in regn)}}});")
            else:  # lane
                for k in range(nr):
                    p.append(f"nx::vst_lane<{es}>(a + {k * es}, c->v[{regn[k]}], {lane});")
            if wb:
                p.append(wb)
        else:
            # Load everything into temporaries first, then write back the base, then the registers.
            p.append("V128 t_[4];")
            if mode == "seq":
                for k in range(nr):
                    p.append(f"t_[{k}] = nx::vld_n<{dsz}>(a + {k * dsz});")
            elif mode == "il":
                p.append(f"nx::vld_il<{n}, {es}, {cnt}>(t_, a);")
            elif mode == "lane":
                for k in range(nr):
                    p.append(f"t_[{k}] = c->v[{regn[k]}]; nx::vld_lane<{es}>(t_[{k}], a + {k * es}, {lane});")
            else:  # repl
                for k in range(nr):
                    p.append(f"t_[{k}] = nx::vld_repl<{es}, {cnt}>(a + {k * es});")
            if wb:
                p.append(wb)
            for k in range(nr):
                p.append(f"c->v[{regn[k]}] = t_[{k}];")
        return "{ " + " ".join(p) + " }"

    def i_ld1(self, f, i, ops):
        return self.xst(i, ops)

    i_ld2 = i_ld3 = i_ld4 = i_ld1
    i_st1 = i_st2 = i_st3 = i_st4 = i_ld1
    i_ld1r = i_ld2r = i_ld3r = i_ld4r = i_ld1

    # ---- unprivileged loads/stores: at EL0 they are exactly LDUR/STUR (signed 9-bit offset, no writeback) ----------

    def i_ldtr(self, f, i, ops):
        return self.ldst(i, ops)

    def i_ldtrb(self, f, i, ops):
        return self.ldst(i, ops, 1)

    def i_ldtrh(self, f, i, ops):
        return self.ldst(i, ops, 2)

    def i_ldtrsb(self, f, i, ops):
        return self.ldst(i, ops, 1, signed=True)

    def i_ldtrsh(self, f, i, ops):
        return self.ldst(i, ops, 2, signed=True)

    def i_ldtrsw(self, f, i, ops):
        return self.ldst(i, ops, 4, signed=True)

    def i_sttr(self, f, i, ops):
        return self.ldst(i, ops, store=True)

    def i_sttrb(self, f, i, ops):
        return self.ldst(i, ops, 1, store=True)

    def i_sttrh(self, f, i, ops):
        return self.ldst(i, ops, 2, store=True)

    # ---- FEAT_LRCPC / FEAT_LRCPC2: LDAPR[BH] and LDAPUR*, STLUR*. LDAPR is an acquire load (nx::ld_acq: RCpc, as the C
    # acquire), STLUR a release store like STLR (nx::st_rel, ctx.h stlr*). LDAPR has no offset; the LRCPC2 forms take a
    # signed unscaled 9-bit byte offset and never write back. ---------

    def lrcpc_addr(self, i, ops):
        regs, mem, after = self.mem_ops(ops)
        if mem is None or after is not None or mem.mem.index or i.writeback:
            raise Unhandled("address form")
        base = Reg(i.reg_name(mem.mem.base))
        a = f"c->x[{base.n}]"
        if mem.mem.disp:
            a = f"({a} + {mem.mem.disp & 0xFFFFFFFFFFFFFFFF:#x}ull)"
        return regs, a

    def lrcpc_ld(self, i, ops, size, signed=False):
        regs, a = self.lrcpc_addr(i, ops)
        d = self.regop(i, regs[0])
        e = f"nx::ld_acq<{u(size * 8)}>({a})"
        if signed:
            e = f"{u(d.bits)}({s(d.bits)}({s(size * 8)}({e})))"
        return d.w(e)

    def lrcpc_st(self, i, ops, size):
        regs, a = self.lrcpc_addr(i, ops)
        r = self.regop(i, regs[0])
        return f"nx::st_rel<{u(size * 8)}>({a}, {u(size * 8)}({r.r()}));"

    def i_ldapr(self, f, i, ops):
        return self.lrcpc_ld(i, ops, self.regop(i, ops[0]).bits // 8)

    i_ldapur = i_ldapr

    def i_ldaprb(self, f, i, ops):
        return self.lrcpc_ld(i, ops, 1)

    i_ldapurb = i_ldaprb

    def i_ldaprh(self, f, i, ops):
        return self.lrcpc_ld(i, ops, 2)

    i_ldapurh = i_ldaprh

    def i_ldapursb(self, f, i, ops):
        return self.lrcpc_ld(i, ops, 1, signed=True)

    def i_ldapursh(self, f, i, ops):
        return self.lrcpc_ld(i, ops, 2, signed=True)

    def i_ldapursw(self, f, i, ops):
        return self.lrcpc_ld(i, ops, 4, signed=True)

    def i_stlur(self, f, i, ops):
        return self.lrcpc_st(i, ops, self.regop(i, ops[0]).bits // 8)

    def i_stlurb(self, f, i, ops):
        return self.lrcpc_st(i, ops, 1)

    def i_stlurh(self, f, i, ops):
        return self.lrcpc_st(i, ops, 2)
