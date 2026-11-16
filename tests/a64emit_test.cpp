#include "../core/src/a64emit.h"
#include <cstdio>
using namespace didj::a64;
int main() {
    static u32 buf[512]; Emitter e; e.init(buf, 512);
    e.mov_imm32(0, 0x12345678); e.mov_imm32(1, 0xFFFFFFF0); e.mov_imm32(2, 0x10000); e.mov_imm64(16, 0x123456789ABCull);
    e.add(0, 1, 2, LSR, 5); e.adds(3, 4, 5); e.subs(31, 1, 2); e.and_(0,1,2,ROR,7); e.ands(0,1,2); e.orr(0,1,2,ASR,31);
    e.eor(0,1,2); e.bic(0,1,2,LSL,3); e.mvn(4,5); e.add_x(7,21,7,LSL,3); e.add_x_uxtw(8,8,9);
    e.adc(1,2,3); e.adcs(1,2,3); e.sbc(1,2,3); e.sbcs(1,2,3); e.add_imm(1,2,4095); e.sub_imm(1,2,1); e.cmp_imm(8,1);
    e.add_imm_x(21,19,256); e.lsl_imm(1,2,5); e.lsr_imm(1,2,5); e.asr_imm(1,2,31); e.ror_imm(1,2,13); e.ubfx(7,1,12,11);
    e.bfi(20,4,30,2); e.sxtb(1,2); e.sxth(1,2); e.lslv(1,2,3); e.lsrv(1,2,3); e.asrv(1,2,3); e.rorv(1,2,3);
    e.mul(1,2,3); e.madd(1,2,3,4); e.umaddl(1,2,3,31); e.smaddl(1,2,3,4); e.clz(1,2); e.csel(1,2,3,GE); e.cset(4,CS);
    e.mrs_nzcv(20); e.msr_nzcv(20); e.ldr_w(1,19,64); e.str_w(1,19,60); e.ldr_x(8,7,8); e.ldrb(8,7,17); e.strb(1,8,0);
    e.ldrh(1,8,0); e.strh(1,8,0); e.ldrsb(1,8,0); e.ldrsh(1,8,0); e.ldr_w_r(0,8,9);
    e.stp_pre(29,30,31,-64); e.stp(19,20,31,16); e.ldp(19,20,31,16); e.ldp_post(29,30,31,64);
    e.tst_low(1,2); e.tst_bit(1,0); e.tst_bit(1,31); e.bic_low(1,1,2); e.orr_bit(2,2,5); e.bic_bit(2,2,5); e.bic_bit(2,2,0); e.bic_bit(2,2,31);
    e.lsl_x_imm(3,3,1); e.lsr_x_imm(3,3,32); e.blr(16); e.ret(); e.ands_x(31,0,0);
    Label l; e.b(l); e.bcond(NE,l); e.cbz(3,l); e.cbnz(3,l); e.tbnz(1,0,l); e.bind(l); e.cbz_x(8,l);
    fwrite(buf, 4, e.n, stdout);
}
