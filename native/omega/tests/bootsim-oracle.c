/* External reference only: original C boot core and causal simulator. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gsp_mmio.h"
#include "gsp_sim.h"
static unsigned char input[2129];
static uint32_t cfg[10], mc[9], trace[1600][3], ri, ne, pauses, latch;
static struct ck_gspsim sim;
static struct ck_gpu_regs device;
static uint32_t u32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void event(uint32_t op,uint32_t off,uint32_t val){if(ne>=1600)exit(3);trace[ne][0]=op;trace[ne][1]=off;trace[ne++][2]=val;}
static uint32_t rd(void *ctx,uint32_t off){(void)ctx;uint32_t v=device.rd(device.ctx,off);ri++;event(0,off,v);return v;}
static void wr(void *ctx,uint32_t off,uint32_t v){(void)ctx;event(1,off,v);device.wr(device.ctx,off,v);if(off==CK_GSP_SEC2_QHEAD0)latch++;}
static void pause_fn(void *ctx){(void)ctx;pauses++;}
static void put(uint32_t v){unsigned char b[4];for(unsigned i=0;i<4;i++)b[i]=(unsigned char)(v>>(8*i));if(fwrite(b,1,4,stdout)!=4)exit(2);}
int main(void){
 size_t n=fread(input,1,sizeof input,stdin);if(ferror(stdin)||n<80||n==sizeof input||memcmp(input,"BTS1",4))return 2;
 for(unsigned i=0;i<10;i++)cfg[i]=u32(input+4+4*i);
 if(cfg[0]>2048||cfg[1]!=0||n!=80+cfg[0]||cfg[9]>1)return 2;
 for(unsigned i=5;i<9;i++)if(cfg[i]>128)return 2;
 for(unsigned i=0;i<9;i++)mc[i]=u32(input+44+cfg[0]+4*i);
 if(mc[6]>1||mc[7]>1)return 2;
 uint64_t pa=(uint64_t)cfg[2]|(uint64_t)cfg[3]<<32;
 struct ck_gspsim_cfg sc={mc[0],mc[1],mc[2],mc[3],mc[4],mc[5],mc[6],mc[7],pa,mc[8]};
 ck_gspsim_init(&sim,&sc);device=ck_gspsim_regs(&sim);
 struct ck_gpu_regs regs={rd,wr,0};
 struct ck_gsp_mmio_in in={input+44,cfg[0],pa,cfg[4],cfg[5],cfg[6],cfg[7],cfg[8],cfg[9]?pause_fn:0,0};
 struct ck_gsp_mmio_out out;int status=ck_gsp_mmio_boot(&regs,&in,&out);if(status!=out.stage)return 3;
 put((uint32_t)-out.stage);for(unsigned i=0;i<4;i++)put(out.polls[i]);
 put(out.dwords);put(out.ememc_after);put(out.sec2_mb0);put(out.sec2_mb1);put(out.hwcfg2);put(out.gsp_mb0);put(out.gsp_mb1);put(out.cpuctl);
 put(latch);put(ri);put(ne);put(pauses);
 for(unsigned i=0;i<ne;i++)for(unsigned j=0;j<3;j++)put(trace[i][j]);
 mc[3]=sim.cfg.args_reads;for(unsigned i=0;i<9;i++)put(mc[i]);
 uint32_t state[]={sim.phase,sim.left,sim.zero_left,sim.qhead,sim.qtail,sim.ememc,sim.sec2_mb0,sim.sec2_mb1,sim.hwcfg2,sim.gsp_mb0,sim.gsp_mb1,sim.cpuctl,sim.os,sim.violations,sim.packet_ok,sim.os_written_at_phase,sim.gsp_qhead_writes};
 for(unsigned i=0;i<17;i++){put(state[i]);}
 for(unsigned i=0;i<256;i++){put(sim.emem[i]);}
 return fflush(stdout)==0?0:2;
}
