/* External test oracle only: links the pinned C reference, never Omega. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gsp_seq.h"
static uint32_t regs[8],out[21];
static uint32_t rd(void*c,uint32_t a){(void)c;out[1]++;return a<8?regs[a]:0;}
static void wr(void*c,uint32_t a,uint32_t v){(void)c;out[2]++;if(a<8)regs[a]=v;}
static void delay(void*c,uint32_t us){(void)c;out[3]+=us;}
static int core(void*c,uint32_t op){(void)op;out[4]++;return *(uint32_t*)c==2?-1:0;}
int main(void){
 uint8_t b[4181];uint32_t in[1045];size_t n=fread(b,1,sizeof b,stdin);
 if(ferror(stdin)||n<84||n%4||n==sizeof b)return 2;
 for(size_t i=0;i<n/4;i++)in[i]=(uint32_t)b[4*i]|((uint32_t)b[4*i+1]<<8)|((uint32_t)b[4*i+2]<<16)|((uint32_t)b[4*i+3]<<24);
 if(in[0]>1024||n!=4*(21+in[0])||in[2]>1024||in[3]>2||in[4]>1)return 2;
 memcpy(regs,in+5,32);gsp_seq_io io={0};io.rd=rd;io.wr=wr;io.ctx=in+3;io.core=in[3]?core:0;io.delay_us=in[4]?delay:0;io.poll_budget=in[2];memcpy(io.save,in+13,32);
 const char*why=0;out[0]=gsp_seq_run(in+21,in[0],in[1],&io,&why)?1:0;
 memcpy(out+5,regs,32);memcpy(out+13,io.save,32);
 for(size_t i=0;i<21;i++)for(size_t j=0;j<4;j++)b[4*i+j]=(uint8_t)(out[i]>>(8*j));
 if(fwrite(b,1,84,stdout)!=84)return 2;return (int)out[0];
}
