/* CLI helpers so the Python cross-check can call the exact C routines.
 *   xcheck ecdh   <priv_hex(21B)> <peer_pub_hex(40B)>   -> shared(20B) hex
 *   xcheck pub    <priv_hex(21B)>                        -> pub(40B) hex
 *   xcheck sesskey <keydata16_hex> <srand16_hex>         -> md5 session key hex
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "uECC.h"

static int unhex(const char *h, uint8_t *o, int max)
{
    int n = 0;
    while (h[0] && h[1] && n < max) { unsigned b; sscanf(h, "%2x", &b); o[n++] = b; h += 2; }
    return n;
}
static void phex(const uint8_t *b, int n) { for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }

/* tiny standalone MD5 so we don't need mbedtls on host */
typedef struct { uint32_t a,b,c,d; uint64_t len; uint8_t buf[64]; size_t n; } md5_t;
static uint32_t rol(uint32_t x,int c){return (x<<c)|(x>>(32-c));}
static void md5_block(md5_t *m,const uint8_t *p){
 static const uint32_t K[64]={
 0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
 0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
 0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
 0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
 0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
 0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
 0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
 0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
 static const int S[64]={7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
 4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
 uint32_t M[16]; for(int i=0;i<16;i++) M[i]=p[i*4]|p[i*4+1]<<8|p[i*4+2]<<16|(uint32_t)p[i*4+3]<<24;
 uint32_t a=m->a,b=m->b,c=m->c,d=m->d;
 for(int i=0;i<64;i++){uint32_t f;int g;
  if(i<16){f=(b&c)|(~b&d);g=i;}
  else if(i<32){f=(d&b)|(~d&c);g=(5*i+1)%16;}
  else if(i<48){f=b^c^d;g=(3*i+5)%16;}
  else{f=c^(b|~d);g=(7*i)%16;}
  f=f+a+K[i]+M[g];a=d;d=c;c=b;b=b+rol(f,S[i]);}
 m->a+=a;m->b+=b;m->c+=c;m->d+=d;
}
static void md5(const uint8_t *in,size_t len,uint8_t out[16]){
 md5_t m={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0,{0},0};
 m.len=len;
 while(len>=64){md5_block(&m,in);in+=64;len-=64;}
 uint8_t tail[128]; memcpy(tail,in,len); tail[len]=0x80;
 size_t pad=(len<56)?56-len:120-len; memset(tail+len+1,0,pad-1);
 uint64_t bits=m.len*8; memcpy(tail+len+pad,&bits,8);
 md5_block(&m,tail); if(len+pad+8>64) md5_block(&m,tail+64);
 uint32_t v[4]={m.a,m.b,m.c,m.d};
 for(int i=0;i<4;i++){out[i*4]=v[i];out[i*4+1]=v[i]>>8;out[i*4+2]=v[i]>>16;out[i*4+3]=v[i]>>24;}
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "pub")) {
        uint8_t priv[21] = {0}, pub[40];
        unhex(argv[2], priv, 21);
        if (uECC_compute_public_key(priv, pub, uECC_secp160r1()) != 1) { puts("ERR"); return 1; }
        phex(pub, 40);
    } else if (!strcmp(argv[1], "ecdh")) {
        uint8_t priv[21] = {0}, peer[40], sec[20];
        unhex(argv[2], priv, 21);
        unhex(argv[3], peer, 40);
        if (uECC_shared_secret(peer, priv, sec, uECC_secp160r1()) != 1) { puts("ERR"); return 1; }
        phex(sec, 20);
    } else if (!strcmp(argv[1], "sesskey")) {
        uint8_t kd[16], sr[16], buf[32], out[16];
        unhex(argv[2], kd, 16);
        unhex(argv[3], sr, 16);
        memcpy(buf, kd, 16); memcpy(buf + 16, sr, 16);
        md5(buf, 32, out);
        phex(out, 16);
    } else if (!strcmp(argv[1], "md5")) {
        uint8_t in[512], out[16];
        int n = unhex(argv[2], in, sizeof in);
        md5(in, n, out);
        phex(out, 16);
    } else if (!strcmp(argv[1], "genkey")) {
        uint8_t priv[21], pub[40];
        uECC_make_key(pub, priv, uECC_secp160r1());
        printf("priv "); phex(priv, 21);
        printf("pub  "); phex(pub, 40);
    } else if (!strcmp(argv[1], "valid")) {
        uint8_t pub[40]; unhex(argv[2], pub, 40);
        printf("%d\n", uECC_valid_public_key(pub, uECC_secp160r1()));
    } else if (!strcmp(argv[1], "recompute")) {
        uint8_t priv[21], pub[40]; unhex(argv[2], priv, 21);
        uECC_compute_public_key(priv, pub, uECC_secp160r1());
        phex(pub, 40);
    }
    return 0;
}

/* appended debug commands via a second translation unit would be cleaner,
   but keep it simple: recompile handles it */
