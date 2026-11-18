/* Exhaust the reference table: past LZ4KDS_REF_MAX every page must still
 * round-trip (fall back to ordinary), and no id may dangle. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lz4kd.h"
#include "lz4kds_sddc.h"
#define BLOCK 4096u
static uint32_t rs=7; static uint32_t rnd(void){rs=rs*1103515245u+12345u;return (rs>>8);}
int main(void){
  struct lz4kds_sddc *s=lz4kds_sddc_create(); struct lz4kds_codec *c=lz4kds_codec_create();
  uint8_t *p=malloc(BLOCK),*o=malloc(2*BLOCK),*b=malloc(BLOCK);
  int fail=0; unsigned n_raw=0,n_ord=0,n_alias=0,n_delta=0;
  if(!s||IS_ERR(s)||!c){printf("FAIL init\n");return 1;}
  for(int t=0;t<20000;t++){
    for(unsigned i=0;i<BLOCK;i++) p[i]=(uint8_t)((t*31+i*7)&0xff);
    unsigned len=2*BLOCK;
    if(lz4kds_sddc_compress(s,c,p,o,&len)){printf("FAIL compress t=%d\n",t);fail++;break;}
    if(len==BLOCK){n_raw++;continue;}
    if(lz4kds_sddc_decompress(s,c,o,len,b)){printf("FAIL dec t=%d k=%02x len=%u\n",t,o[0],len);fail++;break;}
    if(memcmp(p,b,BLOCK)){printf("FAIL cmp t=%d k=%02x\n",t,o[0]);fail++;break;}
    if(o[0]==0x41)n_alias++; else if(o[0]==0x44)n_delta++; else n_ord++;
  }
  printf("exhaust: raw=%u ord=%u alias=%u delta=%u\n",n_raw,n_ord,n_alias,n_delta);
  printf("%s\n",fail?"RESULT: FAIL":"RESULT: PASS");
  return fail?1:0;
}
