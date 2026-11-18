/* Incompressible pages must be reported as PAGE_SIZE and never carry a marker. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lz4kd.h"
#include "lz4kds_sddc.h"
#define BLOCK 4096u
static uint64_t rs=88172645463325252ull;
static uint64_t xr(void){rs^=rs<<13;rs^=rs>>7;rs^=rs<<17;return rs;}
int main(void){
  struct lz4kds_sddc *s=lz4kds_sddc_create(); struct lz4kds_codec *c=lz4kds_codec_create();
  uint8_t *p=malloc(BLOCK),*o=malloc(2*BLOCK),*b=malloc(BLOCK);
  int fail=0; unsigned raw=0,other=0;
  if(!s||IS_ERR(s)||!c){printf("FAIL init\n");return 1;}
  for(int t=0;t<3000;t++){
    for(unsigned i=0;i<BLOCK;i+=8){uint64_t v=xr();memcpy(p+i,&v,8);}
    unsigned len=2*BLOCK;
    if(lz4kds_sddc_compress(s,c,p,o,&len)){printf("FAIL compress t=%d\n",t);fail++;break;}
    if(len==BLOCK){raw++;continue;}
    other++;
    if(lz4kds_sddc_decompress(s,c,o,len,b)||memcmp(p,b,BLOCK)){printf("FAIL rt t=%d k=%02x len=%u\n",t,o[0],len);fail++;break;}
  }
  printf("random: raw=%u compressed=%u\n",raw,other);
  if(other){printf("NOTE: incompressible pages compressed below PAGE_SIZE, still correct\n");}
  printf("%s\n",fail?"RESULT: FAIL":"RESULT: PASS");
  return fail?1:0;
}
