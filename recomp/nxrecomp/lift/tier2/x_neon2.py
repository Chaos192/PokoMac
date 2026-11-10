# SPDX-License-Identifier: GPL-3.0-or-later
"""Tier-2 handlers for one instruction family (neon2). A method named i_<mnemonic> here OVERRIDES the same-named handler of
Wonder's ported mixins (this class comes first in Tier2's base list), so a faulty ported handler is fixed by redefining it
here, and a missing one is added here. Same-named helpers for the generated C++ go in include/nxrecomp/cxx_neon2.h.
Handler signature: i_<mnemonic>(self, f, i, ops) -> one C++ statement (a string); raise Unhandled('why') when a form is
not supported. i is the Capstone instruction (i.address, i.mnemonic, i.op_str, i.operands, i.writeback), ops = i.operands.

Saturating forms use ::v_sat_s / ::v_sat_u (include/nxrecomp/neon.h: exact __int128 result, clamped to the element size)."""
from capstone import arm64_const as A

from .base import B, VAS, Reg, Unhandled, subst, u, s  # noqa: F401


def _sz(bits):
    """Element size code of neon.h's v_sat_s / v_sat_u: 0 = 8 bits, 1 = 16, 2 = 32, 3 = 64."""
    return {8: 0, 16: 1, 32: 2, 64: 3}[bits]


# |a - b| as an unsigned 64-bit value (modular), for signed or unsigned elements
ABD = "a > b ? uint64_t(a) - uint64_t(b) : uint64_t(b) - uint64_t(a)"
ABD_WIDE = "{a} > {b} ? uint64_t({a}) - uint64_t({b}) : uint64_t({b}) - uint64_t({a})"
ABD_ACC = "{o} + (" + ABD_WIDE + ")"


class XNeon2:
    # ---- absolute difference: accumulate, long ---------------------------------------------------------------

    def i_saba(self, f, i, ops):  # Vd += |Vn - Vm| (SABA: modulo the element size)
        return self.v_bin(i, ops, "o + (" + ABD + ")", signed=True)

    def i_uaba(self, f, i, ops):
        return self.v_bin(i, ops, "o + (" + ABD + ")")

    def i_sabd(self, f, i, ops):
        return self.v_bin(i, ops, ABD, signed=True)

    def i_uabd(self, f, i, ops):
        return self.v_bin(i, ops, ABD)

    def i_sabal(self, f, i, ops):  # Vd.2T += |Vn.T - Vm.T|; the `2` variants read the upper halves
        return self.widen(i, ops, ABD_ACC, True)

    i_sabal2 = i_sabal

    def i_uabal(self, f, i, ops):
        return self.widen(i, ops, ABD_ACC, False)

    i_uabal2 = i_uabal

    def i_sabdl(self, f, i, ops):  # widen() formats its template: the operands are {a} and {b}
        return self.widen(i, ops, ABD_WIDE, True)

    i_sabdl2 = i_sabdl

    def i_uabdl(self, f, i, ops):
        return self.widen(i, ops, ABD_WIDE, False)

    i_uabdl2 = i_uabdl

    # ---- long reductions, pairwise accumulate ---------------------------------------------------------------

    def i_saddlv(self, f, i, ops):  # signed sum of all elements into one element of twice the width
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ int64_t s_ = 0; for (int k = 0; k < {n}; k++) s_ += c->v[{a.n}].s{bits}[k]; "
                f"V128 r = nx::vzero(); r.s{bits * 2}[0] = int{bits * 2}_t(s_); c->v[{d.n}] = r; }}")

    def adalp(self, i, ops, signed):
        """SADALP / UADALP: pairwise long add of the source, accumulated into the destination (modulo its width)."""
        dbits, n = self.vinfo(ops[0])
        sbits = dbits // 2
        kind = "s" if signed else "u"
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        keep = "" if dbits * n == 128 else " r.u64[1] = 0;"  # a 64-bit destination has its upper half cleared
        return (f"{{ V128 S = c->v[{a.n}]; V128 r = c->v[{d.n}];{keep} for (int k = 0; k < {n}; k++) "
                f"r.{kind}{dbits}[k] = {'int' if signed else 'uint'}{dbits}_t(uint64_t(r.{kind}{dbits}[k]) + "
                f"uint64_t(S.{kind}{sbits}[2 * k]) + uint64_t(S.{kind}{sbits}[2 * k + 1])); "
                f"c->v[{d.n}] = r; }}")

    def i_sadalp(self, f, i, ops):
        return self.adalp(i, ops, True)

    def i_uadalp(self, f, i, ops):
        return self.adalp(i, ops, False)

    # ---- saturating ------------------------------------------------------------------------------------------

    def i_sqadd(self, f, i, ops):
        return self.v_bin(i, ops, f"::v_sat_s((__int128)a + (__int128)b, {_sz(self.vinfo(ops[0])[0])})", signed=True)

    def i_sqsub(self, f, i, ops):
        return self.v_bin(i, ops, f"::v_sat_s((__int128)a - (__int128)b, {_sz(self.vinfo(ops[0])[0])})", signed=True)

    def i_uqadd(self, f, i, ops):
        return self.v_bin(i, ops, f"::v_sat_u((__int128)a + (__int128)b, {_sz(self.vinfo(ops[0])[0])})")

    def i_sqdmulh(self, f, i, ops):  # sat((2ab) >> esize)
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"::v_sat_s(((__int128)a * (__int128)b * 2) >> {bits}, {_sz(bits)})", signed=True)

    def i_sqrdmulh(self, f, i, ops):  # sat((2ab + 2^(esize-1)) >> esize)
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"::v_sat_s((((__int128)a * (__int128)b) * 2 + ((__int128)1 << {bits - 1})) >> {bits}, "
                                  f"{_sz(bits)})", signed=True)

    def sat_acc(self, i, ops, signed_dst):
        """SUQADD (signed destination + unsigned source, saturated signed) and USQADD (the reverse)."""
        bits, n = self.vinfo(ops[0])
        d, m = self.regop(i, ops[0]), self.regop(i, ops[1])
        if signed_dst:
            e, ty, kind = f"::v_sat_s((__int128)O.s{bits}[k] + (__int128)S.u{bits}[k], {_sz(bits)})", f"int{bits}_t", "s"
        else:
            e, ty, kind = f"::v_sat_u((__int128)O.u{bits}[k] + (__int128)S.s{bits}[k], {_sz(bits)})", f"uint{bits}_t", "u"
        return (f"{{ V128 S = c->v[{m.n}]; V128 O = c->v[{d.n}]; V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.{kind}{bits}[k] = {ty}({e}); c->v[{d.n}] = r; }}")

    def i_suqadd(self, f, i, ops):
        return self.sat_acc(i, ops, True)

    def i_usqadd(self, f, i, ops):
        return self.sat_acc(i, ops, False)

    def i_sqabs(self, f, i, ops):
        return self.v_un(i, ops, f"::v_sat_s(a < 0 ? -(__int128)a : (__int128)a, {_sz(self.vinfo(ops[0])[0])})", signed=True)

    def i_sqneg(self, f, i, ops):
        return self.v_un(i, ops, f"::v_sat_s(-(__int128)a, {_sz(self.vinfo(ops[0])[0])})", signed=True)

    # ---- abs / neg / bit reverse ---------------------------------------------------------------------------

    def i_abs(self, f, i, ops):
        return self.v_un(i, ops, "a < 0 ? uint64_t(0) - uint64_t(a) : uint64_t(a)", signed=True)

    def i_neg(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_un(i, ops, "uint64_t(0) - uint64_t(a)", signed=True)
        bits = d.bits
        return d.w(f"{u(bits)}(0) - {self.shifted(i, ops[1], bits)}")

    def i_rbit(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:  # vector: bits reversed inside each byte
            return self.v_un(i, ops, "nx::rbit8_(a)", lanes="bytes")
        return d.w(f"nx::rbit{d.bits}({self.regop(i, ops[1]).r()})")

    # ---- reciprocal estimates (32-bit elements) ----------------------------------------------------------

    def i_urecpe(self, f, i, ops):
        return self.v_un(i, ops, "nx::urecpe_(a)")

    def i_ursqrte(self, f, i, ops):
        return self.v_un(i, ops, "nx::ursqrte_(a)")

    # ---- polynomial multiply (8-bit) ---------------------------------------------------------------------

    def i_pmul(self, f, i, ops):
        return self.v_bin(i, ops, "nx::pmull8_(a, b)")

    def i_pmull(self, f, i, ops):
        if ops[0].vas == A.ARM64_VAS_1Q:  # 64 x 64 -> 128
            d, a, b = (self.regop(i, o) for o in ops[:3])
            lane = 1 if i.mnemonic == "pmull2" else 0
            return f"c->v[{d.n}] = nx::clmul64(c->v[{a.n}].u64[{lane}], c->v[{b.n}].u64[{lane}]);"
        return self.widen(i, ops, "nx::pmull8_({a}, {b})", False)

    i_pmull2 = i_pmull

    # ---- table lookup ----------------------------------------------------------------------------------------

    def i_tbx(self, f, i, ops):  # like TBL, but an out-of-range index keeps the destination element
        bits, n = self.vinfo(ops[0])
        d = self.regop(i, ops[0])
        tables = [self.regop(i, o) for o in ops[1:-1]]
        idx = self.regop(i, ops[-1])
        cp = " ".join(f"std::memcpy(t_ + {16 * k}, &c->v[{r.n}], 16);" for k, r in enumerate(tables))
        return (f"{{ uint8_t t_[64]; {cp} V128 I_ = c->v[{idx.n}]; V128 O = c->v[{d.n}]; V128 r = nx::vzero(); "
                f"for (int k = 0; k < {n}; k++) r.u8[k] = I_.u8[k] < {16 * len(tables)} ? t_[I_.u8[k]] : O.u8[k]; "
                f"c->v[{d.n}] = r; }}")

    # ---- dot product (ARMv8.2 FEAT_DotProd: the Switch 2's Cortex-A78C has it; Star Fox and Ocarina of Time use UDOT) --------

    def _dot(self, i, ops, signed):
        """UDOT / SDOT Vd.2S|4S, Vn.8B|16B, Vm.8B|16B (or Vm.4B[index]): each 32-bit lane of Vd gains the sum of the four products of
        the bytes in the same lane of Vn and Vm (the by-element form uses Vm's 32-bit element `index`, four bytes, for every lane).
        Bytes widen to 32 bits; the sum and the accumulation wrap modulo 2^32. The 2S form clears the upper half of Vd."""
        bits, n = self.vinfo(ops[0])
        if bits != 32 or n not in (2, 4):
            raise Unhandled("dot product into lanes other than 2S / 4S")
        d, a, b = (self.regop(i, o) for o in ops[:3])
        t = "s" if signed else "u"
        idx = ops[2].vector_index
        m = f"M_.{t}8[{idx} * 4 + q]" if idx != -1 else f"M_.{t}8[k * 4 + q]"
        clear = " r.u64[1] = 0;" if n == 2 else ""
        return (f"{{ V128 N_ = c->v[{a.n}], M_ = c->v[{b.n}], r = c->v[{d.n}]; for (int k = 0; k < {n}; k++) {{ uint32_t s_ = 0; "
                f"for (int q = 0; q < 4; q++) s_ += uint32_t(int32_t(N_.{t}8[k * 4 + q]) * int32_t({m})); r.u32[k] += s_; }}{clear} "
                f"c->v[{d.n}] = r; }}")

    def i_udot(self, f, i, ops):
        return self._dot(i, ops, False)

    def i_sdot(self, f, i, ops):
        return self._dot(i, ops, True)
