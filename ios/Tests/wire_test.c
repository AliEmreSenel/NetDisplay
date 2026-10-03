// SPDX-License-Identifier: GPL-2.0-only
#include "NDNative.h"
#include "proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static int fragment(NDIOSAssembler *a, const uint8_t *bytes, size_t size,
                    uint64_t session, uint32_t seq, uint16_t frag) {
    uint8_t packet[ND_UDP_PAYLOAD_MAX];
    size_t off=(size_t)frag*ND_FRAG_DATA;
    size_t n=size-off; if(n>ND_FRAG_DATA)n=ND_FRAG_DATA;
    struct nd_hdr h={htonl(ND_MAGIC),nd_hton64(session),htonl(seq),htons(frag),
        htons((uint16_t)((size+ND_FRAG_DATA-1)/ND_FRAG_DATA)),htonl((uint32_t)size)};
    memcpy(packet,&h,sizeof(h)); memcpy(packet+sizeof(h),bytes+off,n);
    return nd_ios_assembler_feed(a,packet,sizeof(h)+n);
}
int main(int argc, char **argv) {
    CHECK(nd_ios_init()>=0);
    CHECK(sizeof(struct ndc_hdr)==12 && sizeof(struct ndc_hello)==28);
    CHECK(sizeof(struct ndc_challenge)==20 && sizeof(struct ndc_welcome)==8);
    CHECK(sizeof(struct ndc_display)==76 && sizeof(struct ndc_stream)==84);
    CHECK(ND_FRAG_DATA==1448 && ND_HDR_SIZE==24);
    CHECK(nd_ios_assembler_create(0,NULL)==NULL);
    uint8_t bytes[4200]; for(size_t i=0;i<sizeof(bytes);i++) bytes[i]=(uint8_t)(i*13);
    NDIOSAssembler *a=nd_ios_assembler_create(11,NULL); CHECK(a);
    CHECK(fragment(a,bytes,sizeof(bytes),11,1,1)==0);
    CHECK(fragment(a,bytes,sizeof(bytes),11,1,1)==0);
    CHECK(nd_ios_assembler_stats(a).duplicate==1);
    CHECK(fragment(a,bytes,sizeof(bytes),11,1,0)==0);
    CHECK(fragment(a,bytes,sizeof(bytes),11,1,2)==1);
    CHECK(nd_ios_assembler_stats(a).frame_bytes==sizeof(bytes));
    CHECK(memcmp(bytes,nd_ios_assembler_bytes(a),sizeof(bytes))==0);
    CHECK(fragment(a,bytes,sizeof(bytes),11,1,2)==0);
    CHECK(nd_ios_assembler_stats(a).complete==1);
    CHECK(fragment(a,bytes,sizeof(bytes),11,2,0)==0);
    CHECK(fragment(a,bytes,sizeof(bytes),11,3,0)==0);
    CHECK(nd_ios_assembler_stats(a).partial_drop==1);
    CHECK(fragment(a,bytes,sizeof(bytes),11,2,1)==0);
    CHECK(fragment(a,bytes,sizeof(bytes),12,4,0)==0);
    CHECK(fragment(a,bytes,sizeof(bytes),11,3,1)==0);
    CHECK(fragment(a,bytes,sizeof(bytes),11,3,2)==1);
    CHECK(nd_ios_assembler_stats(a).complete==2);
    uint8_t garbage[ND_UDP_PAYLOAD_MAX+1]={0};
    CHECK(nd_ios_assembler_feed(a,garbage,2)==-1);
    CHECK(nd_ios_assembler_feed(a,garbage,sizeof(garbage))==-1);
    CHECK(nd_ios_assembler_stats(a).bad==2);
    nd_ios_assembler_destroy(a);
    a=nd_ios_assembler_create(11,NULL); CHECK(a);
    CHECK(fragment(a,bytes,10,11,UINT32_MAX,0)==1);
    CHECK(fragment(a,bytes,10,11,0,0)==1);
    CHECK(fragment(a,bytes,10,11,UINT32_MAX,0)==0);
    nd_ios_assembler_destroy(a);

    uint8_t key[32], cn[16], sn[16], out[32], expected[32];
    for(unsigned i=0;i<32;i++)key[i]=(uint8_t)i;
    for(unsigned i=0;i<16;i++){cn[i]=(uint8_t)i;sn[i]=(uint8_t)(i+16);}
    nd_crypto_proof(out,key,"client",cn,sn);
    char hex[65]; nd_crypto_key_to_hex(hex,out);
    // Golden vectors are checked independently in the Python cross-language test.
    if (argc>1) {
        char path[4096]; snprintf(path,sizeof(path),"%s/crypto-vectors.txt",argv[1]);
        FILE *f=fopen(path,"w"); CHECK(f);
        fprintf(f,"client %s\n",hex);
        nd_crypto_proof(out,key,"server",cn,sn);nd_crypto_key_to_hex(hex,out);fprintf(f,"server %s\n",hex);
        nd_crypto_stream_key(out,key,0x0102030405060708ull,cn,sn);nd_crypto_key_to_hex(hex,out);fprintf(f,"stream %s\n",hex);
        fclose(f);
    }
    nd_crypto_proof(out,key,"server",cn,sn); memcpy(expected,out,32);
    CHECK(nd_crypto_verify(out,expected)); expected[0]^=1; CHECK(!nd_crypto_verify(out,expected));
    uint8_t encrypted[4216];size_t n=0;
    CHECK(nd_crypto_encrypt(encrypted,&n,bytes,sizeof(bytes),key,22,7)==0 && n==4216);
    a=nd_ios_assembler_create(22,key); CHECK(a);
    CHECK(fragment(a,encrypted,n,22,7,2)==0);
    CHECK(fragment(a,encrypted,n,22,7,0)==0);
    CHECK(fragment(a,encrypted,n,22,7,1)==1);
    CHECK(nd_ios_assembler_stats(a).frame_bytes==sizeof(bytes));
    CHECK(memcmp(nd_ios_assembler_bytes(a),bytes,sizeof(bytes))==0);
    CHECK(nd_crypto_encrypt(encrypted,&n,bytes,sizeof(bytes),key,22,8)==0);
    encrypted[n-1]^=1;
    CHECK(fragment(a,encrypted,n,22,8,0)==0);
    CHECK(fragment(a,encrypted,n,22,8,1)==0);
    CHECK(fragment(a,encrypted,n,22,8,2)==-1);
    CHECK(nd_ios_assembler_stats(a).bad==1);
    nd_ios_assembler_destroy(a);
    // No undefined behavior on malformed or adversarial random datagrams.
    a=nd_ios_assembler_create(17,NULL); CHECK(a);
    uint32_t rng=12345;
    for(unsigned k=0;k<20000;k++) {
        for(unsigned j=0;j<sizeof(garbage);j++){rng=rng*1664525u+1013904223u;garbage[j]=(uint8_t)(rng>>24);}
        nd_ios_assembler_feed(a,garbage,k%sizeof(garbage));
    }
    nd_ios_assembler_destroy(a);
    if(argc>1) {
        char path[4096]; snprintf(path,sizeof(path),"%s/motion-body.bin",argv[1]);
        FILE *f=fopen(path,"rb"); CHECK(f); uint8_t body[96],tag[32];
        CHECK(fread(body,1,sizeof(body),f)==sizeof(body)); fclose(f);
        nd_ios_hmac(tag,body,sizeof(body),key);
        snprintf(path,sizeof(path),"%s/motion-packet.bin",argv[1]);f=fopen(path,"wb");CHECK(f);
        CHECK(fwrite(body,1,sizeof(body),f)==sizeof(body));CHECK(fwrite(tag,1,sizeof(tag),f)==sizeof(tag));fclose(f);
    }
    printf("PASS: %u C protocol/crypto checks and 20000 malformed-datagram trials\n",checks);
    return 0;
}
