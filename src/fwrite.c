#include "dt_stdio.h"
#include <errno.h>
#include <unistd.h>    // for access()
#include <fcntl.h>
#include <stdbool.h>   // true and false
#include <stdint.h>    // INT32_MIN
#include <math.h>      // isfinite, isnan
#include <stdlib.h>    // abs
#include <string.h>    // strlen, strerror
#ifndef NOZLIB
#include <zlib.h>      // for compression to .gz
#endif

#ifdef WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#define WRITE _write
#define CLOSE _close
#else
#define WRITE write
#define CLOSE close
#endif

#include "myomp.h"
#include "fwriteLookups.h"
#include "fwrite.h"

#define NUM_SF   15
#define SIZE_SF  1000000000000000ULL  // 10^NUM_SF

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

// Globals for this file only. Written once to hold parameters passed from R level.
static const char *na;                 // by default "" or if set (not recommended) then usually "NA"
static char sep;                       // comma in .csv files
static int sepLen;                     // 0 when sep="" for #4817, otherwise 1
static char sep2;                      // '|' within list columns. Used here to know if field should be quoted and in freadR.c to write sep2 in list columns
static char dec;                       // the '.' in the number 3.1416. In Europe often: 3,1416
static int8_t doQuote=INT8_MIN;        // whether to surround fields with double quote ". NA means 'auto' (default)
static bool qmethodEscape=false;       // when quoting fields, how to escape double quotes in the field contents (default false means to add another double quote)
static int scipen;
static bool squashDateTime=false;      // 0=ISO(yyyy-mm-dd) 1=squash(yyyymmdd)
static bool verbose=false;

extern const char *getString(const void *, int64_t);
extern int getStringLen(const void *, int64_t);
extern int getMaxStringLen(const void *, int64_t);
extern int getMaxCategLen(const void *);
extern int getMaxListItemLen(const void *, int64_t);
extern const char *getCategString(const void *, int64_t);
extern double wallclock(void);

inline void write_chars(const char *x, char **pch)
{
  // similar to C's strcpy but i) doesn't include trailing \0 and ii) moves destination along
  char *ch = *pch;
  while (*x) *ch++=*x++;
  *pch = ch;
}

void writeBool8(const void *col, int64_t row, char **pch)
{
  int8_t x = ((const int8_t *)col)[row];
  char *ch = *pch;
  *ch++ = '0'+(x==1);
  *pch = ch-(x==INT8_MIN);  // if NA then step back, to save a branch
}

void writeBool32(const void *col, int64_t row, char **pch)
{
  int32_t x = ((const int32_t *)col)[row];
  char *ch = *pch;
  if (x==INT32_MIN) {  // TODO: when na=='\0' as recommended, use a branchless writer
    write_chars(na, &ch);
  } else {
    *ch++ = '0'+x;
  }
  *pch = ch;
}

void writeBool32AsString(const void *col, int64_t row, char **pch)
{
  int32_t x = ((const int32_t *)col)[row];
  char *ch = *pch;
  if (x == INT32_MIN) {
    write_chars(na, &ch);
  } else if (x) {
    *ch++='T'; *ch++='R'; *ch++='U'; *ch++='E';
  } else {
    *ch++='F'; *ch++='A'; *ch++='L'; *ch++='S'; *ch++='E';
  }
  *pch = ch;
}

static inline void reverse(char *upp, char *low)
{
  upp--;
  while (upp>low) {
    char tmp = *upp;
    *upp = *low;
    *low = tmp;
    upp--;
    low++;
  }
}

void writeInt32(const void *col, int64_t row, char **pch)
{
  char *ch = *pch;
  int32_t x = ((const int32_t *)col)[row];
  if (x == INT32_MIN) {
    write_chars(na, &ch);
  } else {
    if (x<0) { *ch++ = '-'; x=-x; }
    // Avoid log() for speed. Write backwards then reverse when we know how long.
    char *low = ch;
    do { *ch++ = '0'+x%10; x/=10; } while (x>0);
    reverse(ch, low);
  }
  *pch = ch;
}

void writeInt64(const void *col, int64_t row, char **pch)
{
  char *ch = *pch;
  int64_t x = ((const int64_t *)col)[row];
  if (x == INT64_MIN) {
    write_chars(na, &ch);
  } else {
    if (x<0) { *ch++ = '-'; x=-x; }
    char *low = ch;
    do { *ch++ = '0'+x%10; x/=10; } while (x>0);
    reverse(ch, low);
  }
  *pch = ch;
}

/*
 * Generate fwriteLookup.h which defines sigparts, expsig and exppow that writeNumeric() that follows uses.
 * It was run once a long time ago in dev and we don't need to generate it again unless we change it.
 * Commented out and left here in the file where its result is used, in case we need it in future.
 * Reason: ldexpl may not be available on all platforms and is slower than a direct lookup when it is.
 *
void genLookups() {
  FILE *f = fopen("/tmp/fwriteLookups.h", "w");
  fprintf(f, "//\n\
// Generated by fwrite.c:genLookups()\n\
//\n\
// 3 vectors: sigparts, expsig and exppow\n\
// Includes precision higher than double; leave this compiler on this machine\n\
// to parse the literals at reduced precision.\n\
// 2^(-1023:1024) is held more accurately than double provides by storing its\n\
// exponent separately (expsig and exppow)\n\
// We don't want to depend on 'long double' (>64bit) availability to generate\n\
// these at runtime; libraries and hardware vary.\n\
// These small lookup tables are used for speed.\n\
//\n\n");
  fprintf(f, "const double sigparts[53] = {\n0.0,\n");
  for (int i=1; i<=52; i++) {
    fprintf(f, "%.40Le%s\n",ldexpl(1.0L,-i), i==52?"":",");
  }
  fprintf(f, "};\n\nconst double expsig[2048] = {\n");
  char x[2048][60];
  for (int i=0; i<2048; i++) {
    sprintf(x[i], "%.40Le", ldexpl(1.0L, i-1023));
    fprintf(f, "%.*s%s\n", (int)(strchr(x[i],'e')-x[i]), x[i], (i==2047?"":",") );
  }
  fprintf(f, "};\n\nconst int exppow[2048] = {\n");
  for (int i=0; i<2048; i++) {
    fprintf(f, "%d%s", atoi(strchr(x[i],'e')+1), (i==2047?"":",") );
  }
  fprintf(f, "};\n\n");
  fclose(f);
  return R_NilValue;
}
*/

void writeFloat64(const void *col, int64_t row, char **pch)
{
  // hand-rolled / specialized for speed
  // *pch is safely the output destination with enough space (ensured via calculating maxLineLen up front)
  // technique similar to base R (format.c:formatReal and printutils.c:EncodeReal0)
  // differences/tricks :
  //   i) no buffers. writes straight to the final file buffer passed to write()
  //  ii) no C library calls such as sprintf() where the fmt string has to be interpreted over and over
  // iii) no need to return variables or flags.  Just writes.
  //  iv) shorter, easier to read and reason with in one self contained place.
  double x = ((const double *)col)[row];
  char *ch = *pch;
  if (!isfinite(x)) {
    if (isnan(x)) {
      write_chars(na, &ch);
    } else {
      if (x<0) *ch++ = '-';
      *ch++ = 'I'; *ch++ = 'n'; *ch++ = 'f';
    }
  } else if (x == 0.0) {
    *ch++ = '0';   // and we're done.  so much easier rather than passing back special cases
  } else {
    if (x < 0.0) { *ch++ = '-'; x = -x; }  // and we're done on sign, already written. no need to pass back sign
    union { double d; uint64_t l; } u;
    u.d = x;
    uint64_t fraction = u.l & 0xFFFFFFFFFFFFF;           // (1<<52)-1;
    uint32_t exponent = (int32_t)((u.l>>52) & 0x7FF);    // [0,2047]

    // Now sum the appropriate powers 2^-(1:52) of the fraction
    // Important for accuracy to start with the smallest first; i.e. 2^-52
    // Exact powers of 2 (1.0, 2.0, 4.0, etc) are represented precisely with fraction==0
    // Skip over tailing zeros for exactly representable numbers such 0.5, 0.75
    // Underflow here (0u-1u = all 1s) is on an unsigned type which is ok by C standards
    // sigparts[0] arranged to be 0.0 in genLookups() to enable branch free loop here
    double acc = 0;  // 'long double' not needed
    int i = 52;
    if (fraction) {
      while ((fraction & 0xFF) == 0) { fraction >>= 8; i-=8; }
      while (fraction) {
        acc += sigparts[(((fraction & 1u)^1u)-1u) & i];
        i--;
        fraction >>= 1;
      }
    }
    // 1.0+acc is in range [1.5,2.0) by IEEE754
    // expsig is in range [1.0,10.0) by design of fwriteLookups.h
    // Therefore y in range [1.5,20.0)
    // Avoids (potentially inaccurate and potentially slow) log10/log10l, pow/powl, ldexp/ldexpl
    // By design we can just lookup the power from the tables
    double y = (1.0+acc) * expsig[exponent];  // low magnitude mult
    int exp = exppow[exponent];
    if (y>=9.99999999999999) { y /= 10; exp++; }
    uint64_t l = y * SIZE_SF;  // low magnitude mult 10^NUM_SF
    // l now contains NUM_SF+1 digits as integer where repeated /10 below is accurate

    // if (verbose) Rprintf(_("\nTRACE: acc=%.20Le ; y=%.20Le ; l=%"PRIu64" ; e=%d     "), acc, y, l, exp);

    if (l%10 >= 5) l+=10; // use the last digit to round
    l /= 10;
    if (l == 0) {
      if (*(ch-1)=='-') ch--;
      *ch++ = '0';
    } else {
      // Count trailing zeros and therefore s.f. present in l
      int trailZero = 0;
      while (l%10 == 0) { l /= 10; trailZero++; }
      int sf = NUM_SF - trailZero;
      if (sf==0) {sf=1; exp++;}  // e.g. l was 9999999[5-9] rounded to 10000000 which added 1 digit

      // l is now an unsigned long that doesn't start or end with 0
      // sf is the number of digits now in l
      // exp is e<exp> were l to be written with the decimal sep after the first digit
      int dr = sf-exp-1; // how many characters to print to the right of the decimal place
      int width=0;       // field width were it written decimal format. Used to decide whether to or not.
      int dl0=0;         // how many 0's to add to the left of the decimal place before starting l
      if (dr<=0) { dl0=-dr; dr=0; width=sf+dl0; }  // 1, 10, 100, 99000
      else {
        if (sf>dr) width=sf+1;                     // 1.234 and 123.4
        else { dl0=1; width=dr+1+dl0; }            // 0.1234, 0.0001234
      }
      // So:  3.1416 => l=31416, sf=5, exp=0     dr=4; dl0=0; width=6
      //      30460  => l=3046, sf=4, exp=4      dr=0; dl0=1; width=5
      //      0.0072 => l=72, sf=2, exp=-3       dr=4; dl0=1; width=6
      if (width <= sf + (sf>1) + 2 + (abs(exp)>99?3:2) + scipen) {
        //               ^^^^ to not include 1 char for dec in -7e-04 where sf==1
        //                       ^ 2 for 'e+'/'e-'
        // decimal format ...
        ch += width-1;
        if (dr) {
          while (dr && sf) { *ch--='0'+l%10; l/=10; dr--; sf--; }
          while (dr) { *ch--='0'; dr--; }
          *ch-- = dec;
        }
        while (dl0) { *ch--='0'; dl0--; }
        while (sf) { *ch--='0'+l%10; l/=10; sf--; }
        // ch is now 1 before the first char of the field so position it afterward again, and done
        ch += width+1;
      } else {
        // scientific ...
        ch += sf;  // sf-1 + 1 for dec
        for (int i=sf; i>1; i--) {
          *ch-- = '0' + l%10;
          l /= 10;
        }
        if (sf == 1) ch--; else *ch-- = dec;
        *ch = '0' + l;
        ch += sf + (sf>1);
        *ch++ = 'e';  // lower case e to match base::write.csv
        if (exp < 0) { *ch++ = '-'; exp=-exp; }
        else { *ch++ = '+'; }  // to match base::write.csv
        if (exp < 100) {
          *ch++ = '0' + (exp / 10);
          *ch++ = '0' + (exp % 10);
        } else {
          *ch++ = '0' + (exp / 100);
          *ch++ = '0' + (exp / 10) % 10;
          *ch++ = '0' + (exp % 10);
        }
      }
    }
  }
  *pch = ch;
}

void writeComplex(const void *col, int64_t row, char **pch)
{
  Rcomplex x = ((const Rcomplex *)col)[row];
  char *ch = *pch;
  writeFloat64(&x.r, 0, &ch);
  if (!ISNAN(x.i)) {
    if (x.i >= 0.0) *ch++ = '+';  // else writeFloat64 writes the - sign
    writeFloat64(&x.i, 0, &ch);
    *ch++ = 'i';
  }
  *pch = ch;
}

// DATE/TIME

static inline void write_time(int32_t x, char **pch)
// just a helper called below by the real writers (time-only and datetime)
{
  char *ch = *pch;
  if (x<0) {  // <0 covers NA_INTEGER too (==INT_MIN checked in init.c)
    write_chars(na, &ch);
  } else {
    int hh = x/3600;
    int mm = (x - hh*3600) / 60;
    int ss = x%60;
    *ch++ = '0'+hh/10;
    *ch++ = '0'+hh%10;
    *ch++ = ':';
    ch -= squashDateTime;
    *ch++ = '0'+mm/10;
    *ch++ = '0'+mm%10;
    *ch++ = ':';
    ch -= squashDateTime;
    *ch++ = '0'+ss/10;
    *ch++ = '0'+ss%10;
  }
  *pch = ch;
}

void writeITime(const void *col, int64_t row, char **pch) {
  write_time(((const int32_t *)col)[row], pch);
}

static inline void write_date(int32_t x, char **pch)
// just a helper called below by the two real writers (date-only and datetime)
{
  // From base ?Date :
  //  "  Dates are represented as the number of days since 1970-01-01, with negative values
  // for earlier dates. They are always printed following the rules of the current Gregorian calendar,
  // even though that calendar was not in use long ago (it was adopted in 1752 in Great Britain and its
  // colonies)  "

  // The algorithm here in data.table::fwrite was taken from civil_from_days() here :
  //   http://howardhinnant.github.io/date_algorithms.html
  // which was donated to the public domain thanks to Howard Hinnant, 2013.
  // The rebase to 1 March 0000 is inspired: avoids needing isleap() at all.
  // The only small modifications here are :
  //   1) no need for era
  //   2) impose date range of [0000-03-01, 9999-12-31]. All 3,652,365 dates tested in test 1739
  //   3) use direct lookup for mmdd rather than the math using 153, 2 and 5
  //   4) use true/false value (md/100)<3 rather than ?: branch
  // The end result is 5 lines of simple branch free integer math with no library calls.
  // as.integer(as.Date(c("0000-03-01","9999-12-31"))) == c(-719468,+2932896)

  char *ch = *pch;
  if (x< -719468 || x>2932896) {
    // NA_INTEGER<(-719468) (==INT_MIN checked in init.c)
    write_chars(na, &ch);
  } else {
    x += 719468;  // convert days from 1970-01-01 to days from 0000-03-01 (the day after 29 Feb 0000)
    int y = (x - x/1461 + x/36525 - x/146097) / 365;  // year of the preceding March 1st
    int z =  x - y*365 - y/4 + y/100 - y/400 + 1;     // days from March 1st in year y
    int md = monthday[z];  // See fwriteLookups.h for how the 366 item lookup 'monthday' is arranged
    y += z && (md/100)<3;  // The +1 above turned z=-1 to 0 (meaning Feb29 of year y not Jan or Feb of y+1)

    ch += 7 + 2*!squashDateTime;
    *ch-- = '0'+md%10; md/=10;
    *ch-- = '0'+md%10; md/=10;
    *ch-- = '-';
    ch += squashDateTime;
    *ch-- = '0'+md%10; md/=10;
    *ch-- = '0'+md%10; md/=10;
    *ch-- = '-';
    ch += squashDateTime;
    *ch-- = '0'+y%10; y/=10;
    *ch-- = '0'+y%10; y/=10;
    *ch-- = '0'+y%10; y/=10;
    *ch   = '0'+y%10; y/=10;
    ch += 8 + 2*!squashDateTime;
  }
  *pch = ch;
}

void writeDateInt32(const void *col, int64_t row, char **pch) {
  write_date(((const int32_t *)col)[row], pch);
}

void writeDateFloat64(const void *col, int64_t row, char **pch) {
  double x = ((const double *)col)[row];
  write_date(isfinite(x) ? (int)(x) : INT32_MIN, pch);
}

void writePOSIXct(const void *col, int64_t row, char **pch)
{
  // Write ISO8601 UTC by default to encourage ISO standards, stymie ambiguity and for speed.
  // R internally represents POSIX datetime in UTC always. Its 'tzone' attribute can be ignored.
  // R's representation ignores leap seconds too which is POSIX compliant, convenient and fast.
  // Aside: an often overlooked option for users is to start R in UTC: $ TZ='UTC' R
  // All positive integers up to 2^53 (9e15) are exactly representable by double which is relied
  // on in the ops here; number of seconds since epoch.

  double x = ((const double *)col)[row];
  char *ch = *pch;
  if (!isfinite(x)) {
    write_chars(na, &ch);
  } else {
    int64_t xi, d, t;
    xi = floor(x);
    int m = ((x-xi)*10000000); // 7th digit used to round up if 9
    m += (m%10);  // 9 is numerical accuracy, 8 or less then we truncate to last microsecond
    m /= 10;
    int carry = m / 1000000; // Need to know if we rounded up to a whole second
    m -= carry * 1000000;
    xi += carry;
    if (xi>=0) {
      d = xi / 86400;
      t = xi % 86400;
    } else {
      // before 1970-01-01T00:00:00Z
      d = (xi+1)/86400 - 1;
      t = xi - d*86400;  // xi and d are both negative here; t becomes the positive number of seconds into the day
    }
    write_date(d, &ch);
    *ch++ = 'T';
    ch -= squashDateTime;
    write_time(t, &ch);
    if (squashDateTime || (m && m%1000==0)) {
      // when squashDateTime always write 3 digits of milliseconds even if 000, for consistent scale of squash integer64
      // don't use writeInteger() because it doesn't 0 pad which we need here
      // integer64 is big enough for squash with milli but not micro; trunc (not round) micro when squash
      m /= 1000;
      *ch++ = '.';
      ch -= squashDateTime;
      *(ch+2) = '0'+m%10; m/=10;
      *(ch+1) = '0'+m%10; m/=10;
      *ch     = '0'+m;
      ch += 3;
    } else if (m) {
      // microseconds are present and !squashDateTime
      *ch++ = '.';
      *(ch+5) = '0'+m%10; m/=10;
      *(ch+4) = '0'+m%10; m/=10;
      *(ch+3) = '0'+m%10; m/=10;
      *(ch+2) = '0'+m%10; m/=10;
      *(ch+1) = '0'+m%10; m/=10;
      *ch     = '0'+m;
      ch += 6;
    }
    *ch++ = 'Z';
    ch -= squashDateTime;
  }
  *pch = ch;
}

void writeNanotime(const void *col, int64_t row, char **pch)
{
  int64_t x = ((const int64_t *)col)[row];
  char *ch = *pch;
  if (x == INT64_MIN) {
    write_chars(na, &ch);
  } else {
    int d/*days*/, s/*secs*/, n/*nanos*/;
    n = x % 1000000000;
    x /= 1000000000;
    if (x>=0 && n>=0) {
      d = x / 86400;
      s = x % 86400;
    } else {
      // before 1970-01-01T00:00:00.000000000Z
      if (n) { x--; n += 1000000000; }
      d = (x+1)/86400 - 1;
      s = x - d*86400;  // x and d are both negative here; secs becomes the positive number of seconds into the day
    }
    write_date(d, &ch);
    *ch++ = 'T';
    ch -= squashDateTime;
    write_time(s, &ch);
    *ch++ = '.';
    ch -= squashDateTime;
    for (int i=8; i>=0; i--) { *(ch+i) = '0'+n%10; n/=10; }  // always 9 digits for nanoseconds
    ch += 9;
    *ch++ = 'Z';
    ch -= squashDateTime;
  }
  *pch = ch;
}

static inline void write_string(const char *x, char **pch)
{
  char *ch = *pch;
  if (x == NULL) {
    // NA is not quoted even when quote=TRUE to distinguish from quoted "NA" value.  But going forward: ,,==NA and ,"",==empty string
    write_chars(na, &ch);
  } else {
    int8_t q = doQuote;
    if (q==INT8_MIN) { // NA means quote="auto"
      const char *tt = x;
      if (*tt=='\0') {
        // Empty strings are always quoted to distinguish from ,,==NA
        *ch++='"'; *ch++='"';   // test 1732.7 covers this (confirmed in gdb) so it's unknown why codecov claims no coverage
        *pch = ch;
        return;
      }
      while (*tt!='\0' && *tt!=sep && *tt!=sep2 && *tt!='\n' && *tt!='\r' && *tt!='"') *ch++ = *tt++;
      // Windows includes \n in its \r\n so looking for \n only is sufficient
      // sep2 is set to '\0' when no list columns are present
      if (*tt=='\0') {
        // most common case: no sep, newline or " contained in string
        *pch = ch;  // advance caller over the field already written
        return;
      }
      ch = *pch; // rewind the field written since it needs to be quoted
      q = true;
    }
    if (q==false) {
      write_chars(x, &ch);
    } else {
      *ch++ = '"';
      const char *tt = x;
      if (qmethodEscape) {
        while (*tt!='\0') {
          if (*tt=='"' || *tt=='\\') *ch++ = '\\';
          *ch++ = *tt++;
        }
      } else {
        // qmethod='double'
        while (*tt!='\0') {
          if (*tt=='"') *ch++ = '"';
          *ch++ = *tt++;
        }
      }
      *ch++ = '"';
    }
  }
  *pch = ch;
}

void writeString(const void *col, int64_t row, char **pch)
{
  write_string(getString((const SEXP *)col, row), pch);
}

void writeCategString(const void *col, int64_t row, char **pch)
{
  write_string(getCategString((const SEXP *)col, row), pch);
}

#ifndef NOZLIB
int init_stream(z_stream *stream) {
  stream->next_in = Z_NULL;
  stream->zalloc = Z_NULL;
  stream->zfree = Z_NULL;
  stream->opaque = Z_NULL;

  int err = deflateInit2(stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
  return err;  // # nocov
}

int compressbuff(z_stream *stream, void* dest, size_t *destLen, const void* source, size_t sourceLen)
{
  stream->next_out = dest;
  stream->avail_out = *destLen;
  stream->next_in = (Bytef *)source; // don't use z_const anywhere; #3939
  stream->avail_in = sourceLen;
  int err = deflate(stream, Z_SYNC_FLUSH);
  *destLen = *destLen - stream->avail_out;
  // *destLen = stream->total_out;
  return err != Z_STREAM_ERROR ? Z_OK : err;
}
#endif

// main fwrite function ----
void fwriteMain(fwriteMainArgs args)
{
  double startTime = wallclock();
  double nextTime = startTime+2; // start printing progress meter in 2 sec if not completed by then

  na = args.na;
  sep = args.sep;
  sepLen = sep=='\0' ? 0 : 1;
  sep2 = args.sep2;
  dec = args.dec;
  scipen = args.scipen;
  doQuote = args.doQuote;
  int8_t quoteHeaders = args.doQuote;
  verbose = args.verbose;

  size_t len;
  unsigned int crc;

  // When NA is a non-empty string, then we must quote all string fields in case they contain the na string
  // na is recommended to be empty, though
  if (na[0]!='\0' && doQuote==INT8_MIN) doQuote = true;

  qmethodEscape = args.qmethodEscape;
  squashDateTime = args.squashDateTime;

  if (args.buffMB<1 || args.buffMB>1024) STOP(_("buffMB=%d outside [1,1024]"), args.buffMB);
  size_t buffSize = (size_t)1024*1024*args.buffMB;

  int eolLen=strlen(args.eol), naLen=strlen(args.na);
  // Aside: codacy wants strnlen but strnlen is not in C99 (neither is strlen_s). To pass `gcc -std=c99 -Wall -pedantic`
  //        we'd need `#define _POSIX_C_SOURCE 200809L` before #include <string.h> but that seems a step too far
  //        and platform specific. We prefer to be pure C99.
  if (eolLen<=0) STOP(_("eol must be 1 or more bytes (usually either \\n or \\r\\n) but is length %d"), eolLen);

  if (verbose) {
    DTPRINT(_("Column writers: "));
    if (args.ncol<=50) {
      for (int j=0; j<args.ncol; j++) DTPRINT(_("%d "), args.whichFun[j]);
    } else {
      for (int j=0; j<30; j++) DTPRINT(_("%d "), args.whichFun[j]);
      DTPRINT(_("... "));
      for (int j=args.ncol-10; j<args.ncol; j++) DTPRINT(_("%d "), args.whichFun[j]);
    }
    DTPRINT(_("\nargs.doRowNames=%d args.rowNames=%p args.rowNameFun=%d doQuote=%d args.nrow=%"PRId64" args.ncol=%d eolLen=%d\n"),
          args.doRowNames, args.rowNames, args.rowNameFun, doQuote, args.nrow, args.ncol, eolLen);
  }

  // Calculate upper bound for line length. Numbers use a fixed maximum (e.g. 12 for integer) while strings find the longest
  // string in each column. Upper bound is then the sum of the columns' max widths.
  // This upper bound is required to determine a reasonable rowsPerBatch. It also saves needing to grow the buffers which
  // is especially tricky when compressing, and saves needing to check/limit the buffer writing because we know
  // up front the buffer does have sufficient capacity.
  // A large overestimate (e.g. 2-5x too big) is ok, provided it is not so large that the buffers can't be allocated.
  // Do this first so that, for example, any unsupported types in list columns happen first before opening file (which
  // could be console output) and writing column names to it.

  double t0 = wallclock();
  size_t maxLineLen = eolLen + args.ncol*(2*(doQuote!=0) + sepLen);
  if (args.doRowNames) {
    maxLineLen += args.rowNames==NULL ? 1+(int)log10(args.nrow)   // the width of the row number
                  : (args.rowNameFun==WF_String ? getMaxStringLen(args.rowNames, args.nrow)*2  // *2 in case longest row name is all quotes (!) and all get escaped
                  : 11); // specific integer names could be MAX_INT 2147483647 (10 chars) even on a 5 row table, and data.frame allows negative integer rownames hence 11 for the sign
    maxLineLen += 2/*possible quotes*/ + sepLen;
  }
  for (int j=0; j<args.ncol; j++) {
    int width = writerMaxLen[args.whichFun[j]];
    if (width==0) {
      switch(args.whichFun[j]) {
      case WF_String:
        width = getMaxStringLen(args.columns[j], args.nrow);
        break;
      case WF_CategString:
        width = getMaxCategLen(args.columns[j]);
        break;
      case WF_List:
        width = getMaxListItemLen(args.columns[j], args.nrow);
        break;
      default:
        STOP(_("Internal error: type %d has no max length method implemented"), args.whichFun[j]);  // # nocov
      }
    }
    if (args.whichFun[j]==WF_Float64 && args.scipen>0) width+=MIN(args.scipen,350); // clamp width to IEEE754 max to avoid scipen=99999 allocating buffer larger than can ever be written
    if (width<naLen) width = naLen;
    maxLineLen += width*2;  // *2 in case the longest string is all quotes and they all need to be escaped
  }
  if (verbose) DTPRINT(_("maxLineLen=%"PRIu64". Found in %.3fs\n"), (uint64_t)maxLineLen, 1.0*(wallclock()-t0));

  int f=0;
  if (*args.filename=='\0') {
    f=-1;  // file="" means write to standard output
    args.is_gzip = false; // gzip is only for file
    // eol = "\n";  // We'll use DTPRINT which converts \n to \r\n inside it on Windows
  } else {
#ifdef WIN32
    f = _open(args.filename, _O_WRONLY | _O_BINARY | _O_CREAT | (args.append ? _O_APPEND : _O_TRUNC), _S_IWRITE);
    // O_BINARY rather than O_TEXT for explicit control and speed since it seems that write() has a branch inside it
    // to convert \n to \r\n on Windows when in text mode not not when in binary mode.
#else
    f = open(args.filename, O_WRONLY | O_CREAT | (args.append ? O_APPEND : O_TRUNC), 0666);
    // There is no binary/text mode distinction on Linux and Mac
#endif
    if (f == -1) {
      // # nocov start
      int erropen = errno;
      STOP(access( args.filename, F_OK ) != -1 ?
           _("%s: '%s'. Failed to open existing file for writing. Do you have write permission to it? Is this Windows and does another process such as Excel have it open?") :
           _("%s: '%s'. Unable to create new file for writing (it does not exist already). Do you have permission to write here, is there space on the disk and does the path exist?"),
           strerror(erropen), args.filename);
      // # nocov end
    }
  }
#ifdef NOZLIB
  if (args.is_gzip)
    STOP(_("Compression in fwrite uses zlib library. Its header files were not found at the time data.table was compiled. To enable fwrite compression, please reinstall data.table and study the output for further guidance.")); // # nocov
#endif

  int yamlLen = strlen(args.yaml);
  if (verbose) {
    DTPRINT(_("Writing bom (%s), yaml (%d characters) and column names (%s) ... "),
            args.bom?"true":"false", yamlLen, args.colNames?"true":"false");
    if (f==-1) DTPRINT(_("\n"));
  }
  size_t headerLen = 0;
  if (args.bom) headerLen += 3;
  headerLen += yamlLen;
  if (args.colNames) {
    for (int j=0; j<args.ncol; j++) headerLen += getStringLen(args.colNames, j)*2;  // *2 in case quotes are escaped or doubled
    headerLen += args.ncol*(sepLen+(doQuote!=0)*2) + eolLen + 3;  // 3 in case doRowNames and doQuote (the first blank <<"",>> column name)
  }
  if (headerLen) {
    char *buff = malloc(headerLen);
    if (!buff)
      STOP(_("Unable to allocate %zu MiB for header: %s"), headerLen / 1024 / 1024, strerror(errno));
    char *ch = buff;
    if (args.bom) {*ch++=(char)0xEF; *ch++=(char)0xBB; *ch++=(char)0xBF; }  // 3 appears above (search for "bom")
    memcpy(ch, args.yaml, yamlLen);
    ch += yamlLen;
    if (args.colNames) {
      if (args.doRowNames) {
        // Unusual: the extra blank column name when row_names are added as the first column
        if (doQuote!=0/*'auto'(NA) or true*/) { *ch++='"'; *ch++='"'; } // to match write.csv
        *ch = sep;
        ch += sepLen;
      }
      int8_t tempDoQuote = doQuote;
      doQuote = quoteHeaders; // temporary overwrite since headers might get different quoting behavior, #2964
      for (int j=0; j<args.ncol; j++) {
        writeString(args.colNames, j, &ch);
        *ch = sep;
        ch += sepLen;
      }
      doQuote = tempDoQuote;
      ch -= sepLen; // backup over the last sep
      write_chars(args.eol, &ch);
    }
    if (f==-1) {
      *ch = '\0';
      DTPRINT("%s", buff);
      free(buff);
    } else {
      int ret0=0, ret1=0, ret2=0;
      if (args.is_gzip) {
#ifndef NOZLIB
        z_stream stream;
        if(init_stream(&stream)) {
          free(buff);                                    // # nocov
          STOP(_("Can't allocate gzip stream structure"));  // # nocov
        }
        // by default, buffsize is the same used for writing rows (#5048 old openbsd zlib)
        // takes the max with headerLen size in case of very long header
        size_t zbuffSize = deflateBound(&stream, headerLen > buffSize ? headerLen : buffSize);
        char *zbuff = malloc(zbuffSize);
        if (!zbuff) {
          free(buff);                                                                                   // # nocov
          STOP(_("Unable to allocate %zu MiB for zbuffer: %s"), zbuffSize / 1024 / 1024, strerror(errno));  // # nocov
        }
        // write minimal gzip header
        char* header = "\037\213\10\0\0\0\0\0\0\3";
        ret0 = WRITE(f, header, 10);
        crc = crc32(0L, Z_NULL, 0);

        size_t zbuffUsed = zbuffSize;
        len = (size_t)(ch - buff);
        crc = crc32(crc, buff, len);
        ret1 = compressbuff(&stream, zbuff, &zbuffUsed, buff, len);
        if (ret1==Z_OK)
            ret2 = WRITE(f, zbuff, (int)zbuffUsed);
        free(zbuff);
#endif
      } else {
        ret2 = WRITE(f,  buff, (int)(ch-buff));
      }
      free(buff);
      if (ret0 == -1 || ret1 || ret2==-1) {
        // # nocov start
        int errwrite = errno; // capture write errno now in case close fails with a different errno
        CLOSE(f);
        if (ret0 == -1) STOP(_("Can't write gzip header error: %d"), ret0);
        else if (ret1) STOP(_("Compress gzip error: %d"), ret1);
        else STOP(_("%s: '%s'"), strerror(errwrite), args.filename);
        // # nocov end
      }
    }
  }
  if (verbose) DTPRINT(_("done in %.3fs\n"), 1.0*(wallclock()-t0));
  if (args.nrow == 0) {
    if (verbose) DTPRINT(_("No data rows present (nrow==0)\n"));
    if (f!=-1 && CLOSE(f)) STOP(_("%s: '%s'"), strerror(errno), args.filename);
    return;
  }

  // Writing rows

  // Decide buffer size and rowsPerBatch for each thread
  // Once rowsPerBatch is decided it can't be changed
  int rowsPerBatch=0;
  if (2 * maxLineLen > buffSize) {
      buffSize=2*maxLineLen;
      rowsPerBatch=2;
  }
  else
      rowsPerBatch = buffSize / maxLineLen;
  if (rowsPerBatch > args.nrow)
      rowsPerBatch = args.nrow;
  if (rowsPerBatch < 1)
      rowsPerBatch = 1;
  int numBatches = (args.nrow-1) / rowsPerBatch + 1;
  int nth = args.nth;
  if (numBatches < nth)
      nth = numBatches;
  if (verbose) {
    DTPRINT(_("Writing %"PRId64" rows in %d batches of %d rows (each buffer size %dMB, showProgress=%d, nth=%d)\n"),
            args.nrow, numBatches, rowsPerBatch, args.buffMB, args.showProgress, nth);
  }
  t0 = wallclock();

  bool hasPrinted = false;
  int maxBuffUsedPC = 0;

  // compute zbuffSize which is the same for each thread
  size_t zbuffSize = 0;
  if(args.is_gzip){
#ifndef NOZLIB
    z_stream stream;
    if(init_stream(&stream))
        STOP(_("Can't allocate gzip stream structure")); // # nocov
    zbuffSize = deflateBound(&stream, buffSize);
    if (verbose)
        DTPRINT(_("zbuffSize=%d returned from deflateBound\n"), (int)zbuffSize);
#endif
  }

  errno=0;
  char *buffPool = malloc(nth*(size_t)buffSize);
  if (!buffPool) {
    // # nocov start
    STOP(_("Unable to allocate %zu MB * %d thread buffers; '%d: %s'. Please read ?fwrite for nThread, buffMB and verbose options."),
         (size_t)buffSize/(1024^2), nth, errno, strerror(errno));
    // # nocov end
  }
  char *zbuffPool = NULL;
  if (args.is_gzip) {
#ifndef NOZLIB
    zbuffPool = malloc(nth*(size_t)zbuffSize);
    if (!zbuffPool) {
      // # nocov start
      free(buffPool);
      STOP(_("Unable to allocate %zu MB * %d thread compressed buffers; '%d: %s'. Please read ?fwrite for nThread, buffMB and verbose options."),
         (size_t)zbuffSize/(1024^2), nth, errno, strerror(errno));
      // # nocov end
    }
#endif
  }

#ifndef NOZLIB
  z_stream *thread_streams = (z_stream*) malloc(nth * sizeof(z_stream));
  if (!thread_streams)
    STOP(_("Failed to allocated %d bytes for threads_streams."), (int)(nth * sizeof(z_stream)));
  // VLA on stack should be fine for nth structs; in zlib v1.2.11 sizeof(struct)==112 on 64bit
  // not declared inside the parallel region because solaris appears to move the struct in
  // memory when the #pragma omp for is entered, which causes zlib's internal self reference
  // pointer to mismatch, #4099
#endif

bool failed = false;   // naked (unprotected by atomic) write to bool ok because only ever write true in this special paradigm
int failed_compress = 0; // the first thread to fail writes their reason here when they first get to ordered section
int failed_write = 0;    // same. could use +ve and -ve in the same code but separate it out to trace Solaris problem, #3931

// loop overs rows in parallel----
#pragma omp parallel for ordered num_threads(nth)
    for(int64_t start=0; start < args.nrow; start += rowsPerBatch) {
        int me = omp_get_thread_num();
        size_t mylen = 0;
        int mycrc;
        int my_failed_compress = 0;
        char* myBuff = buffPool + me * buffSize;
        char* ch = myBuff;

#ifndef NOZLIB
        z_stream *mystream = &thread_streams[me];
        void *myzBuff = NULL;
        size_t myzbuffUsed = 0;
        if (args.is_gzip) {
            myzBuff = zbuffPool + me * zbuffSize;
            if (init_stream(mystream)) { // this should be thread safe according to zlib documentation
                failed = true;              // # nocov
                my_failed_compress = -998;  // # nocov
            }
        }
#endif
      if (failed)
          continue;  // Not break. Because we don't use #omp cancel yet.
      int64_t end = ((args.nrow - start) < rowsPerBatch) ? args.nrow : start + rowsPerBatch;

      // chunk rows
      for (int64_t i=start; i<end; i++) {
        // Tepid starts here (once at beginning of each line)
        if (args.doRowNames) {
          if (args.rowNames==NULL) {
            if (doQuote==1) *ch++='"';
            int64_t rn = i+1;
            writeInt64(&rn, 0, &ch);
            if (doQuote==1) *ch++='"';
          } else {
            if (args.rowNameFun != WF_String && doQuote==1) *ch++='"';
            (args.funs[args.rowNameFun])(args.rowNames, i, &ch);  // #5098
            if (args.rowNameFun != WF_String && doQuote==1) *ch++='"';
          }
          *ch = sep;
          ch += sepLen;
        }
        // Hot loop
        for (int j=0; j<args.ncol; j++) {
          (args.funs[args.whichFun[j]])(args.columns[j], i, &ch);
          *ch = sep;
          ch += sepLen;
        }
        // Tepid again (once at the end of each line)
        ch -= sepLen;  // backup onto the last sep after the last column. ncol>=1 because 0-columns was caught earlier.
        write_chars(args.eol, &ch);  // overwrite last sep with eol instead
      } // end of chunk rows loop

      // compress buffer if gzip
#ifndef NOZLIB
      if (args.is_gzip && !failed) {
        myzbuffUsed = zbuffSize;
        mylen = (size_t)(ch - myBuff);
        mycrc = crc32(0, myBuff, mylen);
        int ret = compressbuff(mystream, myzBuff, &myzbuffUsed, myBuff, mylen);
        if (ret) {
            failed=true;
            my_failed_compress=ret;
        }
      }
#endif

      // ordered region ----
#pragma omp ordered
      if (failed) {
        // # nocov start
        if (failed_compress==0 && my_failed_compress!=0) {
          failed_compress = my_failed_compress;
        }
        // else another thread could have failed below while I was working or waiting above; their reason got here first
        // # nocov end
      } else {
        errno=0;
        int ret = 0;
        if (f==-1) {
          *ch='\0';  // standard C string end marker so DTPRINT knows where to stop
          DTPRINT("%s", myBuff);
        } else if (args.is_gzip) {
            ret = WRITE(f, myzBuff, (int)myzbuffUsed);
        } else {
            ret = WRITE(f, myBuff,  (int)(ch-myBuff));
        }
        if (ret == -1) {
          failed=true;         // # nocov
          failed_write=errno;  // # nocov
        }

        crc = crc32_combine(crc, mycrc, mylen);
        len += mylen;

        int used = 100*((double)(ch-myBuff))/buffSize;  // percentage of original buffMB
        if (used > maxBuffUsedPC) maxBuffUsedPC = used;
        double now;
        if (me==0 && args.showProgress && (now=wallclock())>=nextTime && !failed) {
          // See comments above inside the f==-1 clause.
          // Not only is this ordered section one-at-a-time but we'll also Rprintf() here only from the
          // master thread (me==0) and hopefully this will work on Windows. If not, user should set
          // showProgress=FALSE until this can be fixed or removed.
          // # nocov start
          int ETA = (int)((args.nrow-end)*((now-startTime)/end));
          if (hasPrinted || ETA >= 2) {
            if (verbose && !hasPrinted) DTPRINT("\n");
            DTPRINT("\rWritten %.1f%% of %"PRId64" rows in %d secs using %d thread%s. "
                    "maxBuffUsed=%d%%. ETA %d secs.      ",
                     (100.0*end)/args.nrow, args.nrow, (int)(now-startTime), nth, nth==1?"":"s",
                     maxBuffUsedPC, ETA);
            // TODO: use progress() as in fread
            nextTime = now+1;
            hasPrinted = true;
          }
          // # nocov end
        }
      }
    } // end of parallel for loop

/* put a 4-byte integer into a byte array in LSB order */
#define PUT4(a,b) ((a)[0]=(b), (a)[1]=(b)>>8, (a)[2]=(b)>>16, (a)[3]=(b)>>24)

  // write gzip tailer with crc and len
    if (args.is_gzip) {
        // DTPRINT(_("crc=%x len=%ld\n", crc, len));
        unsigned char tail[10];
        tail[0] = 3;
        tail[1] = 0;
        PUT4(tail + 2, crc);
        PUT4(tail + 6, len);
        int ret = WRITE(f, tail, 10);
        if (ret == -1)
            STOP("Error: can't write gzip tailer");
    }

  free(buffPool);
#ifndef NOZLIB
  free(thread_streams);
  free(zbuffPool);
#endif

  // Finished parallel region and can call R API safely now.
  if (hasPrinted) {
    // # nocov start
    if (!failed) { // clear the progress meter
      DTPRINT("\r                                                                       "
              "                                                              \r");
    } else {       // don't clear any potentially helpful output before error
      DTPRINT("\n");
    }
    // # nocov end
  }

  if (f!=-1 && CLOSE(f) && !failed)
    STOP("%s: '%s'", strerror(errno), args.filename);  // # nocov
  // quoted '%s' in case of trailing spaces in the filename
  // If a write failed, the line above tries close() to clean up, but that might fail as well. So the
  // '&& !failed' is to not report the error as just 'closing file' but the next line for more detail
  // from the original error.
  if (failed) {
    // # nocov start
#ifndef NOZLIB
    if (failed_compress)
      STOP(_("zlib %s (zlib.h %s) deflate() returned error %d Z_FINISH=%d Z_BLOCK=%d. %s"),
           zlibVersion(), ZLIB_VERSION, failed_compress, Z_FINISH, Z_BLOCK,
           verbose ? _("Please include the full output above and below this message in your data.table bug report.")
                   : _("Please retry fwrite() with verbose=TRUE and include the full output with your data.table bug report."));
#endif
    if (failed_write)
      STOP("%s: '%s'", strerror(failed_write), args.filename);
    // # nocov end
  }
}
