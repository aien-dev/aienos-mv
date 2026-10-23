/* External oracle invokes the unchanged full legacy arena/boot builder. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "gb10_gsp_boot.h"
static unsigned char input[829],bl[GSP_GB10B_BOOTLOADER_SIZE],fmc[8192],sys[GSP_SYSTEM_INFO_SIZE];
static uint32_t u32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put64(uint64_t v){for(unsigned i=0;i<8;i++)if(putchar((int)((v>>(i*8))&255))==EOF)exit(2);}
int main(void){
 size_t n=fread(input,1,sizeof input,stdin);if(n!=828||ferror(stdin))return 2;
 uint64_t pa=(uint64_t)u32(input)|((uint64_t)u32(input+4)<<32);uint32_t size=u32(input+8);if(!size||size>sizeof fmc)return 2;
 void *mem=aligned_alloc(4096,8*1024*1024);if(!mem)return 2;
 gsp_arena arena;gsp_boot_plan plan;gsp_boot_in in={0};const char *why=0;unsigned char elf=1,sig=2;
 if(gsp_arena_init(&arena,mem,pa,8*1024*1024)){free(mem);return 2;}
 in.rm_elf=&elf;in.rm_elf_size=1;in.rm_sig=&sig;in.rm_sig_size=1;
 in.bootloader=bl;in.bootloader_size=sizeof bl;in.bl_code_off=GSP_GB10B_BL_CODE_OFF;in.bl_data_off=GSP_GB10B_BL_DATA_OFF;in.bl_manifest_off=GSP_GB10B_BL_MANIFEST_OFF;
 in.fmc=fmc;in.fmc_size=size;in.fmc_hash=input+12;in.fmc_pubkey=input+60;in.fmc_sig=input+444;
 in.sysmem_heap_pa=0x80000000;in.sysmem_heap_size=4096;in.system_info=sys;in.dma_bits=64;
 if(gsp_boot_build(&arena,&in,&plan,&why)){fprintf(stderr,"builder refused: %s\n",why);free(mem);return 3;}
 if(plan.cot_packet_len!=868||plan.emem_queue_tail!=864){free(mem);return 3;}
 put64(plan.fmc.pa);put64(plan.fmc_params.pa);int ok=fwrite(plan.cot_packet,1,868,stdout)==868;free(mem);return ok&&fflush(stdout)==0?0:2;
}
