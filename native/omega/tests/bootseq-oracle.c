/* External C oracle only. Never linked into native Omega. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gsp_mmio.h"
static unsigned char input[10285];
static uint32_t cfg[10], trace[1600][3], ri, ne, pauses, latch;
static uint32_t u32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void event(uint32_t op,uint32_t off,uint32_t val){if(ne>=1600)exit(3);trace[ne][0]=op;trace[ne][1]=off;trace[ne++][2]=val;}
static uint32_t rd(void *ctx,uint32_t off){(void)ctx;if(ri>=cfg[1])exit(3);const unsigned char *p=input+44+cfg[0]+8*ri++;if(u32(p)!=off)exit(3);uint32_t v=u32(p+4);event(0,off,v);return v;}
static void wr(void *ctx,uint32_t off,uint32_t v){(void)ctx;event(1,off,v);if(off==CK_GSP_SEC2_QHEAD0)latch++;}
static void pause_fn(void *ctx){(void)ctx;pauses++;}
static void put(uint32_t v){unsigned char b[4];for(unsigned i=0;i<4;i++)b[i]=(unsigned char)(v>>(8*i));if(fwrite(b,1,4,stdout)!=4)exit(2);}
int main(void){
 size_t n=fread(input,1,sizeof input,stdin);if(ferror(stdin)||n<44||n==sizeof input||memcmp(input,"BTP1",4))return 2;
 for(unsigned i=0;i<10;i++)cfg[i]=u32(input+4+4*i);
 if(cfg[0]>2048||cfg[1]>1024||n!=44+cfg[0]+8*cfg[1]||cfg[9]>1)return 2;
 for(unsigned i=5;i<9;i++)if(cfg[i]>128)return 2;
 struct ck_gpu_regs regs={rd,wr,0};
 struct ck_gsp_mmio_in in={input+44,cfg[0],(uint64_t)cfg[2]|(uint64_t)cfg[3]<<32,cfg[4],cfg[5],cfg[6],cfg[7],cfg[8],cfg[9]?pause_fn:0,0};
 struct ck_gsp_mmio_out out;int status=ck_gsp_mmio_boot(&regs,&in,&out);
 if(status!=out.stage)return 3;
 put((uint32_t)-out.stage);for(unsigned i=0;i<4;i++)put(out.polls[i]);
 put(out.dwords);put(out.ememc_after);put(out.sec2_mb0);put(out.sec2_mb1);put(out.hwcfg2);put(out.gsp_mb0);put(out.gsp_mb1);put(out.cpuctl);
 put(latch);put(ri);put(ne);put(pauses);
 for(unsigned i=0;i<ne;i++)for(unsigned j=0;j<3;j++)put(trace[i][j]);
 return fflush(stdout)==0?0:2;
}
