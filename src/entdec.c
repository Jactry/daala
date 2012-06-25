/* Copyright (c) 2001-2011 Timothy B. Terriberry
   Copyright (c) 2008-2009 Xiph.Org Foundation */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include "entdec.h"

/*A range decoder.
  This is an entropy decoder based upon \cite{Mar79}, which is itself a
   rediscovery of the FIFO arithmetic code introduced by \cite{Pas76}.
  It is very similar to arithmetic encoding, except that encoding is done with
   digits in any base, instead of with bits, and so it is faster when using
   larger bases (i.e.: a byte).
  The author claims an average waste of $\frac{1}{2}\log_b(2b)$ bits, where $b$
   is the base, longer than the theoretical optimum, but to my knowledge there
   is no published justification for this claim.
  This only seems true when using near-infinite precision arithmetic so that
   the process is carried out with no rounding errors.

  An excellent description of implementation details is available at
   http://www.arturocampos.com/ac_range.html
  A recent work \cite{MNW98} which proposes several changes to arithmetic
   encoding for efficiency actually re-discovers many of the principles
   behind range encoding, and presents a good theoretical analysis of them.

  End of stream is handled by writing out the smallest number of bits that
   ensures that the stream will be correctly decoded regardless of the value of
   any subsequent bits.
  od_ec_tell() can be used to determine how many bits were needed to decode
   all the symbols thus far; other data can be packed in the remaining bits of
   the input buffer.
  @PHDTHESIS{Pas76,
    author="Richard Clark Pasco",
    title="Source coding algorithms for fast data compression",
    school="Dept. of Electrical Engineering, Stanford University",
    address="Stanford, CA",
    month=May,
    year=1976
  }
  @INPROCEEDINGS{Mar79,
   author="Martin, G.N.N.",
   title="Range encoding: an algorithm for removing redundancy from a digitised
    message",
   booktitle="Video & Data Recording Conference",
   year=1979,
   address="Southampton",
   month=Jul
  }
  @ARTICLE{MNW98,
   author="Alistair Moffat and Radford Neal and Ian H. Witten",
   title="Arithmetic Coding Revisited",
   journal="{ACM} Transactions on Information Systems",
   year=1998,
   volume=16,
   number=3,
   pages="256--294",
   month=Jul,
   URL="http://www.stanford.edu/class/ee398/handouts/papers/Moffat98ArithmCoding.pdf"
  }*/



/*This is meant to be a large, positive constant that can still be efficiently
   loaded as an immediate (on platforms like ARM, for example).
  Even relatively modest values like 100 would work fine.*/
#define OD_EC_LOTS_OF_BITS (0x4000)



/*Takes updated dif and range values, renormalizes them so that
   32768<=rng<65536 (reading more bytes from the stream into dif if necessary),
   and stores them back in the decoder context.
  _dif: The new value of dif.
  _rng: The new value of the range.
  _ret: The value to return.
  Return: _ret.
          This allows the compiler to jump to this function via a tail-call.*/
static int od_ec_dec_normalize(od_ec_dec *_this,
 od_ec_window _dif,unsigned _rng,int _ret){
  int nbits_total;
  int d;
  int c;
  int s;
  nbits_total=_this->nbits_total;
  c=_this->cnt;
  d=16-OD_ILOG_NZ(_rng);
  c-=d;
  _dif<<=d;
  if(c<0){
    const unsigned char *buf;
    ogg_uint32_t         storage;
    ogg_uint32_t         offs;
    buf=_this->buf;
    storage=_this->storage;
    offs=_this->offs;
    for(s=OD_EC_WINDOW_SIZE-9-(c+15);s>=0;){
      if(offs>=storage){
        c=OD_EC_LOTS_OF_BITS;
        break;
      }
      _dif|=(od_ec_window)buf[offs++]<<s;
      c+=8;
      s-=8;
    }
    _this->offs=offs;
  }
  _this->nbits_total=nbits_total+d;
  _this->val=_dif;
  _this->rng=_rng<<d;
  _this->cnt=c;
  return _ret;
}


/*Initializes the decoder.
  _buf: The input buffer to use.
  Return: 0 on success, or a negative value on error.*/
void od_ec_dec_init(od_ec_dec *_this,
 unsigned char *_buf,ogg_uint32_t _storage){
  od_ec_window dif;
  ogg_uint32_t offs;
  int          c;
  int          s;
  offs=0;
  dif=0;
  c=-15;
  for(s=OD_EC_WINDOW_SIZE-9;s>=0;){
    if(offs>=_storage){
      c=OD_EC_LOTS_OF_BITS;
      break;
    }
    c+=8;
    dif|=(od_ec_window)_buf[offs++]<<s;
    s-=8;
  }
  _this->buf=_buf;
  _this->storage=_storage;
  _this->end_offs=0;
  _this->end_window=0;
  _this->nend_bits=0;
  /*We reserve one bit for termination.*/
  _this->nbits_total=1;
  _this->offs=offs;
  _this->rng=0x8000;
  _this->cnt=c;
  _this->val=dif;
  _this->error=0;
}

/*Calculates the scaled cumulative frequency for the next symbol given the
   total frequency count.
  This can then be fed into the probability model to determine what that
   symbol is, and the additional frequency information required to advance to
   the next symbol.
  This function cannot be called more than once without a corresponding call to
   od_ec_dec_update(), or decoding will not proceed correctly.
  _ft: The total frequency of the symbols in the alphabet the next symbol was
        encoded with.
       This must be at least 16384 and no more than 32767.
  Return: A cumulative frequency representing the encoded symbol.
          If the cumulative frequency of all the symbols before the one that
           was encoded was fl, and the cumulative frequency of all the symbols
           up to and including the one encoded is fh, then the returned value
           will fall in the range [fl,fh).*/
unsigned od_ec_decode_normalized(od_ec_dec *_this,unsigned _ft){
  unsigned dif;
  unsigned r;
  unsigned d;
  int      s;
  dif=(unsigned)(_this->val>>OD_EC_WINDOW_SIZE-16);
  r=_this->rng;
  s=r>=_ft<<1;
  _ft<<=s;
  d=r-_ft;
  _this->ext=s;
  return OD_MAXI((int)(dif>>1),(int)(dif-d))>>s;
}

/*Calculates the cumulative frequency for the next symbol given a total
   frequency count with an arbitrary scale.
  This function cannot be called more than once without a corresponding call to
   od_ec_dec_update(), or decoding will not proceed correctly.
  _ft: The total frequency of the symbols in the alphabet the next symbol was
        encoded with.
       This must be no more than 32767.
  Return: A cumulative frequency representing the encoded symbol.*/
unsigned od_ec_decode(od_ec_dec *_this,unsigned _ft){
  unsigned dif;
  unsigned r;
  unsigned d;
  int      s;
  dif=(unsigned)(_this->val>>OD_EC_WINDOW_SIZE-16);
  r=_this->rng;
  s=15-OD_ILOG_NZ(_ft-1);
  _ft<<=s;
  if(r>=_ft<<1){
    _ft<<=1;
    s++;
  }
  d=r-_ft;
  _this->ext=s;
  return OD_MAXI((int)(dif>>1),(int)(dif-d))>>s;
}

/*Equivalent to od_ec_decode_normalized() with _ft==32768 (which is normally
   disallowed due to possible 16-bit int overflow).
  This function cannot be called more than once without a corresponding call to
   od_ec_dec_update(), or decoding will not proceed correctly.
  Return: A cumulative frequency representing the encoded symbol.*/
unsigned od_ec_decode_bin_normalized(od_ec_dec *_this){
  unsigned dif;
  unsigned r;
  unsigned d;
  dif=(unsigned)(_this->val>>OD_EC_WINDOW_SIZE-16);
  r=_this->rng;
  d=r-32768U;
  _this->ext=0;
  return OD_MAXI((int)(dif>>1),(int)(dif-d));
}

/*Equivalent to od_ec_decode() with _ft==1<<_ftb (except that _ftb may be as
   large as 15).
  This function cannot be called more than once without a corresponding call to
   od_ec_dec_update(), or decoding will not proceed correctly.
  Return: A cumulative frequency representing the encoded symbol.*/
unsigned od_ec_decode_bin(od_ec_dec *_this,unsigned _ftb){
  unsigned dif;
  unsigned r;
  unsigned d;
  int      s;
  dif=(unsigned)(_this->val>>OD_EC_WINDOW_SIZE-16);
  r=_this->rng;
  d=r-32768U;
  s=15-_ftb;
  _this->ext=s;
  return OD_MAXI((int)(dif>>1),(int)(dif-d))>>s;
}

/*Advance the decoder past the next symbol using the frequency information the
   symbol was encoded with.
  Exactly one call to od_ec_decode() must have been made so that all necessary
   intermediate calculations are performed.
  _fl:  The cumulative frequency of all symbols that come before the symbol
         decoded.
  _fh:  The cumulative frequency of all symbols up to and including the symbol
         decoded.
        Together with _fl, this defines the range [_fl,_fh) in which the value
         returned above must fall.
  _ft:  The total frequency of the symbols in the alphabet the symbol decoded
         was encoded in.
        This must be the same as passed to the preceding call to
         od_ec_decode() (or the equivalent of what would have been passed, if
         another variant of that function was used).*/
void od_ec_dec_update(od_ec_dec *_this,unsigned _fl,unsigned _fh,unsigned _ft){
  od_ec_window dif;
  unsigned     r;
  unsigned     d;
  unsigned     u;
  unsigned     v;
  int          s;
  dif=_this->val;
  r=_this->rng;
  s=(int)_this->ext;
  _fl<<=s;
  _fh<<=s;
  _ft<<=s;
  d=r-_ft;
  u=_fl+OD_MINI(_fl,d);
  v=_fh+OD_MINI(_fh,d);
  r=v-u;
  dif-=(od_ec_window)(u<<OD_EC_WINDOW_SIZE-16);
  od_ec_dec_normalize(_this,dif,r,0);
}

/*Decode a bit that has a 1/(1<<_logp) probability of being a one.
  No corresponding call to od_ec_dec_update() is necessary after this call.
  _logp: The negative base-2 log of the probability of being a one.
         This must be no more than 15.
  Return: The value decoded (0 or 1).*/
int od_ec_dec_bit_logp(od_ec_dec *_this,unsigned _logp){
  od_ec_window dif;
  od_ec_window vw;
  unsigned     r;
  unsigned     v;
  int          ret;
  dif=_this->val;
  r=_this->rng;
  v=32768U-(1<<15-_logp);
  v+=OD_MINI(v,r-32768U);
  vw=(od_ec_window)v<<OD_EC_WINDOW_SIZE-16;
  ret=dif>=vw;
  if(ret)dif-=vw;
  r=ret?r-v:v;
  return od_ec_dec_normalize(_this,dif,r,ret);
}

/*Decodes a symbol given an "inverse" CDF table.
  No corresponding call to od_ec_dec_update() is necessary after this call.
  _icdf: The "inverse" CDF, such that symbol s falls in the range
          [s>0?ft-_icdf[s-1]:0,ft-_icdf[s]), where ft=1<<_ftb.
         The values must be monotonically non-increasing, and the last value
          must be 0.
  _ft: The total of the cumulative distribution.
       This must be no more than 32767.
  Return: The decoded symbol s.*/
int od_ec_dec_icdf_ft(od_ec_dec *_this,
 const unsigned char *_icdf,unsigned _ft){
  od_ec_window dif;
  unsigned     r;
  unsigned     d;
  int          s;
  unsigned     u;
  unsigned     v;
  unsigned     q;
  unsigned     fl;
  unsigned     fh;
  unsigned     ft;
  int          ret;
  dif=_this->val;
  r=_this->rng;
  s=15-OD_ILOG_NZ(_ft-1);
  ft=_ft<<s;
  if(r>=ft<<1){
    ft<<=1;
    s++;
  }
  d=r-ft;
  q=OD_MAXI((int)(dif>>OD_EC_WINDOW_SIZE-15),
   (int)((dif>>OD_EC_WINDOW_SIZE-16)-d))>>s;
  q=_ft-q;
  fl=_ft;
  for(ret=0;_icdf[ret]>=q;ret++)fl=_icdf[ret];
  fl=_ft-fl<<s;
  fh=_ft-_icdf[ret]<<s;
  u=fl+OD_MINI(fl,d);
  v=fh+OD_MINI(fh,d);
  r=v-u;
  dif-=(od_ec_window)u<<OD_EC_WINDOW_SIZE-16;
  return od_ec_dec_normalize(_this,dif,r,ret);
}

/*Decodes a symbol given an "inverse" CDF table.
  No corresponding call to od_ec_dec_update() is necessary after this call.
  _icdf: The "inverse" CDF, such that symbol s falls in the range
          [s>0?ft-_icdf[s-1]:0,ft-_icdf[s]), where ft=1<<_ftb.
         The values must be monotonically non-increasing, and the last value
          must be 0.
  _ft: The total of the cumulative distribution.
       This must be no more than 32767.
  Return: The decoded symbol s.*/
int od_ec_dec_icdf16_ft(od_ec_dec *_this,
 const ogg_uint16_t *_icdf,unsigned _ft){
  od_ec_window dif;
  unsigned     r;
  unsigned     d;
  int          s;
  unsigned     u;
  unsigned     v;
  unsigned     q;
  unsigned     fl;
  unsigned     fh;
  unsigned     ft;
  int          ret;
  dif=_this->val;
  r=_this->rng;
  s=15-OD_ILOG_NZ(_ft-1);
  ft=_ft<<s;
  if(r>=ft<<1){
    ft<<=1;
    s++;
  }
  d=r-ft;
  q=OD_MAXI((int)(dif>>OD_EC_WINDOW_SIZE-15),
   (int)((dif>>OD_EC_WINDOW_SIZE-16)-d))>>s;
  q=_ft-q;
  fl=_ft;
  for(ret=0;_icdf[ret]>=q;ret++)fl=_icdf[ret];
  fl=_ft-fl<<s;
  fh=_ft-_icdf[ret]<<s;
  u=fl+OD_MINI(fl,d);
  v=fh+OD_MINI(fh,d);
  r=v-u;
  dif-=(od_ec_window)u<<OD_EC_WINDOW_SIZE-16;
  return od_ec_dec_normalize(_this,dif,r,ret);
}

/*Decodes a symbol given an "inverse" CDF table.
  No corresponding call to od_ec_dec_update() is necessary after this call.
  _icdf: The "inverse" CDF, such that symbol s falls in the range
          [s>0?ft-_icdf[s-1]:0,ft-_icdf[s]), where ft=1<<_ftb.
         The values must be monotonically non-increasing, and the last value
          must be 0.
  _ftb: The number of bits of precision in the cumulative distribution.
        This must be no more than 15.
  Return: The decoded symbol s.*/
int od_ec_dec_icdf(od_ec_dec *_this,const unsigned char *_icdf,unsigned _ftb){
  od_ec_window dif;
  unsigned     r;
  unsigned     d;
  int          s;
  unsigned     u;
  unsigned     v;
  unsigned     q;
  unsigned     fl;
  unsigned     fh;
  int          ret;
  dif=_this->val;
  r=_this->rng;
  s=15-_ftb;
  d=r-32768U;
  q=OD_MAXI((int)(dif>>OD_EC_WINDOW_SIZE-15),
   (int)((dif>>OD_EC_WINDOW_SIZE-16)-d))>>s;
  fl=(1U<<_ftb);
  q=fl-q;
  for(ret=0;_icdf[ret]>=q;ret++)fl=_icdf[ret];
  fl=32768U-(fl<<s);
  fh=32768U-(_icdf[ret]<<s);
  u=fl+OD_MINI(fl,d);
  v=fh+OD_MINI(fh,d);
  r=v-u;
  dif-=(od_ec_window)u<<OD_EC_WINDOW_SIZE-16;
  return od_ec_dec_normalize(_this,dif,r,ret);
}

/*Decodes a symbol given an "inverse" CDF table.
  No corresponding call to od_ec_dec_update() is necessary after this call.
  _icdf: The "inverse" CDF, such that symbol s falls in the range
          [s>0?ft-_icdf[s-1]:0,ft-_icdf[s]), where ft=1<<_ftb.
         The values must be monotonically non-increasing, and the last value
          must be 0.
  _ftb: The number of bits of precision in the cumulative distribution.
        This must be no more than 15.
  Return: The decoded symbol s.*/
int od_ec_dec_icdf16(od_ec_dec *_this,const ogg_uint16_t *_icdf,unsigned _ftb){
  od_ec_window dif;
  unsigned     r;
  unsigned     d;
  int          s;
  unsigned     u;
  unsigned     v;
  unsigned     q;
  unsigned     fl;
  unsigned     fh;
  int          ret;
  dif=_this->val;
  r=_this->rng;
  s=15-_ftb;
  d=r-32768U;
  q=OD_MAXI((int)(dif>>OD_EC_WINDOW_SIZE-15),
   (int)((dif>>OD_EC_WINDOW_SIZE-16)-d))>>s;
  fl=(1U<<_ftb);
  q=fl-q;
  for(ret=0;_icdf[ret]>=q;ret++)fl=_icdf[ret];
  fl=32768U-(fl<<s);
  fh=32768U-(_icdf[ret]<<s);
  u=fl+OD_MINI(fl,d);
  v=fh+OD_MINI(fh,d);
  r=v-u;
  dif-=(od_ec_window)u<<OD_EC_WINDOW_SIZE-16;
  return od_ec_dec_normalize(_this,dif,r,ret);
}

/*Extracts a raw unsigned integer with a non-power-of-2 range from the stream.
  The integer must have been encoded with od_ec_enc_uint().
  No corresponding call to od_ec_dec_update() is necessary after this call.
  _ft: The number of integers that can be decoded (one more than the max).
       This must be at least one, and no more than 2**32-1.
  Return: The decoded bits.*/
ogg_uint32_t od_ec_dec_uint(od_ec_dec *_this,ogg_uint32_t _ft){
  unsigned fs;
  if(_ft>1U<<OD_EC_UINT_BITS){
    ogg_uint32_t t;
    unsigned     ft;
    int          ftb;
    _ft--;
    ftb=OD_ILOG_NZ(_ft)-OD_EC_UINT_BITS;
    ft=(unsigned)(_ft>>ftb)+1<<15-OD_EC_UINT_BITS;
    fs=od_ec_decode_normalized(_this,ft)&~((1<<15-OD_EC_UINT_BITS)-1);
    od_ec_dec_update(_this,fs,fs+(1<<15-OD_EC_UINT_BITS),ft);
    t=(ogg_uint32_t)(fs>>15-OD_EC_UINT_BITS)<<ftb|od_ec_dec_bits(_this,ftb);
    if(t<=_ft)return t;
    _this->error=1;
    return _ft;
  }
  else{
    fs=od_ec_decode(_this,_ft);
    od_ec_dec_update(_this,fs,fs+1,_ft);
    return fs;
  }
}

/*Extracts a sequence of raw bits from the stream.
  The bits must have been encoded with od_ec_enc_bits().
  No corresponding call to od_ec_dec_update() is necessary after this call.
  _ftb: The number of bits to extract.
        This must be between 0 and 25, inclusive.
  Return: The decoded bits.*/
ogg_uint32_t od_ec_dec_bits(od_ec_dec *_this,unsigned _bits){
  od_ec_window window;
  int          available;
  ogg_uint32_t ret;
  window=_this->end_window;
  available=_this->nend_bits;
  if((unsigned)available<_bits){
    const unsigned char *buf;
    ogg_uint32_t         end_offs;
    ogg_uint32_t         storage;
    buf=_this->buf;
    end_offs=_this->end_offs;
    storage=_this->storage;
    do{
      if(end_offs>=storage){
        available=OD_EC_LOTS_OF_BITS;
        break;
      }
      window|=buf[storage-++end_offs]<<available;
      available+=8;
    }
    while(available<=OD_EC_WINDOW_SIZE-8);
    _this->end_offs=end_offs;
  }
  ret=(ogg_uint32_t)window&((ogg_uint32_t)1<<_bits)-1U;
  window>>=_bits;
  available-=_bits;
  _this->end_window=window;
  _this->nend_bits=available;
  _this->nbits_total+=_bits;
  return ret;
}
