// BASE.C -- common base class implementation

#include "BASE.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ugens.h>
#include <mixerr.h>
#include "common.h"
#include <rt.h>
#include <rtdefs.h>

#undef debug
#undef SIG_DEBUG
#undef LOOP_DEBUG

#ifdef SIG_DEBUG
#define DBG(stmt) { stmt; }
#else
#define DBG(stmt)
#endif

#if defined(SIG_DEBUG) || defined(LOOP_DEBUG)
#define DBG1(stmt) { stmt; }
#else
#define DBG1(stmt)
#endif

extern "C" {
   #include "cmixfuns.h"
   #include "setup.h"
}

extern int g_Nterms[13];                 /* defined in common.C */

int BASE::primes[NPRIMES + 2];
int BASE::primes_gotten = 0;

BASE::BASE()
{
    in = NULL;
    m_tapDelay = NULL;
	m_buffersize = BUFLEN;

    for (int i = 0; i < 2; i++) {
       int j;
       for (j = 0; j < 6; j++)
          m_rvbData[i][j].Rvb_del = NULL;
       for (j = 0; j < 13; j++) {
          m_vectors[i][j].Firtaps = NULL;
          m_vectors[i][j].Fircoeffs = NULL;
       }
    }
    get_primes(NPRIMES, primes);        /* load array with prime numbers */
}

BASE::~BASE()
{
   delete [] in;
   delete [] m_tapDelay;
   int i, j;
   for (i = 0; i < 2; i++) {
      for (j = 0; j < 6; j++)
         delete [] m_rvbData[i][j].Rvb_del;
      for (j = 0; j < 13; j++) {
         delete [] m_vectors[i][j].Firtaps;
         delete [] m_vectors[i][j].Fircoeffs;
      }
   }
}

int BASE::init(float p[], int n_args)
{
    int    flag, UseMikes, cartflag = 0;
    float  outskip, inskip, abs_factor, rvb_time;
    double R, T, dist;


    outskip = p[0];
    inskip = p[1];
    m_dur = p[2];
    if (m_dur < 0)                      /* "dur" represents timend */
        m_dur = -m_dur - inskip;

    rtsetinput(inskip, this);
    insamps = (int)(m_dur * SR);
    inamp = p[3];
   

    double Matrix[12][12];
   
    /* Get results of Minc setup calls (space, mikes_on, mikes_off, matrix) */
    if (get_setup_params(Dimensions, Matrix, &abs_factor, &rvb_time,
                         &UseMikes, &MikeAngle, &MikePatternFactor) == -1)
       die(name(), "You must call setup routine `space' first.");

    localInit(p, n_args);	// call inst-specific init code

    wire_matrix(Matrix);

    /* (perform some initialization that used to be in space.c) */
    int meanLength = MFP_samps(Dimensions);   /* mean delay length for reverb */
    get_lengths(meanLength);              /* sets up delay lengths */
    alloc_delays();                     /* allocates memory for delays */
    set_gains(rvb_time);                /* sets gains for filters */
    set_walls(abs_factor);              /* sets wall filts for move routine */
    set_allpass();
    set_random();                       /* sets up random variation of delays */

    if (m_inchan >= inputchans)
       die(name(), "You asked for channel %d of a %d-channel input file.",
                                                         m_inchan, inputchans);
    if (inputchans == 1)
       m_inchan = 0;

   m_tapDelay = new double[m_tapsize + 8];
   memset(m_tapDelay, 0, (m_tapsize + 8) * sizeof(double));
#ifdef DELAY_DEBUG
   printf("tap delay has length %d\n", (m_tapsize + 8));
#endif

   rvb_reset(m_tapDelay);                  // resets reverb & tap delay

   /* flag for use of ear filters */
   m_binaural = (!UseMikes && m_dist < 0.8 && m_dist != 0.0);
   if (m_binaural) {
      advise(name(), "Running in binaural mode.");
      alloc_firfilters();                     // allocates memory for FIRs
   }

   if (outputchans != 2)
      die(name(), "Output must be stereo.");

   amparray = floc(1);
   if (amparray) {
      int amplen = fsize(1);
      tableset(m_dur, amplen, amptabs);          /* controls input dur only */
   }
   else
      advise(name(), "Setting phrase curve to all 1's.");

   skip = (int)(SR / (float)resetval);
   
   /* determine extra run time for this routine before calling rtsetoutput() */
   double ringdur = 0.0;
   finishInit(rvb_time, &ringdur);
   
   nsamps = rtsetoutput(outskip, m_dur + ringdur, this);
   DBG1(printf("nsamps = %d\n", nsamps));
   return nsamps;
}

void PrintInput(float *sig, int len)
{
    for (int i = 0; i < len; i++)
	    if (sig[i] != 0.0)
		    printf("sig[%d] = %f\n", i, sig[i]);
	printf("\n");
}

void PrintSig(double *sig, int len, double threshold = 0.0)
{
    for (int i = 0; i < len; i++)
	    if (sig[i] > threshold || sig[i] < -threshold)
		    printf("sig[%d] = %f\n", i, sig[i]);
	printf("\n");
}

int BASE::getInput(int currentSample, int frames)
{
    // number of samples to process this time through

    int rsamps = frames * inputchans;

    rtgetin(in, this, rsamps);

    float aamp = inamp;                  /* in case amparray == NULL */

    int n = 0, branch = 0;
    int bufsamps = BUFLEN;	// adjust
    int lCurSamp;	// local copy for inner loops
    float insig;
#ifdef LOOP_DEBUG
    int nsig = 0, nzeros = 0;
#endif

    // apply curve to input signal and mix down to mono if necessary

    for (int s = 0, lCurSamp = currentSample; s < rsamps; s += inputchans, lCurSamp++)
    {
		if (lCurSamp < insamps) {	/* processing input signal */
#ifdef LOOP_DEBUG
			nsig++;
#endif
			if (--branch < 0) {
			   if (amparray)
    			  aamp = tablei(lCurSamp, amparray, amptabs) * inamp;
			   branch = skip;
			}
			if (m_inchan == AVERAGE_CHANS) {
			   insig = 0.0;
			   for (int c = 0; c < inputchans; c++)
    			  insig += in[s + c];
			   insig /= (float)inputchans;
			}
			else
			   insig = in[s + m_inchan];
			insig *= aamp;
		}
		else  {                               /* flushing delays & reverb */
#ifdef LOOP_DEBUG
			nzeros++;
#endif
 			insig = 0.0;
		}
	    in[n++] = insig;	// write back into input array to save space
    }
	DBG1(printf("getInput(): %d signal, %d zero padded\n", nsig, nzeros));
	return 0;
}

/* ------------------------------------------------------------------ run --- */
int BASE::run()
{
	int    i = 0;
	double roomsig[2][BUFLEN], rvbsig[2][BUFLEN];

    if (in == NULL)                /* first time, so allocate it */
       in = new float [RTBUFSAMPS * inputchans];

    Instrument::run();
    
	const int totalSamps = insamps + tapcount;

	DBG1(printf("%s::run(): totalSamps = %d\n", name(), totalSamps));

	// this will return chunksamps' worth of input, even if we have
	// passed the end of the input (will produce zeros)

	getInput(cursamp, chunksamps);

	DBG1(printf("getInput(%d, %d) called\n", cursamp, chunksamps));
	
	int bufsamps = getBufferSize();
	
	// loop for required number of output samples

	while (i < chunksamps) {
		// limit buffer size to end of current pull (chunksamps)
        if (chunksamps - i < bufsamps)
            bufsamps = max(0, chunksamps - i);

		DBG1(printf("top of main loop: i = %d  cursamp = %d  bufsamps = %d\n",
                   i, cursamp, bufsamps));
		DBG(printf("input signal:\n"));
		DBG(PrintInput(&in[i], bufsamps));
		
		// add signal to delay
		put_tap(cursamp, &in[i], bufsamps);

		// if processing input signal or flushing delay lines ... 

		if (cursamp < totalSamps) {
		    // limit buffer size of end of input data
			if (totalSamps - cursamp < bufsamps)
			    bufsamps = max(0, totalSamps - cursamp);
			
		    if ((tapcount = updatePosition(cursamp)) < 0)
			    exit(-1);

			DBG1(printf("  inner loop: bufsamps = %d\n", bufsamps));
		
			for (int ch = 0; ch < 2; ch++) {
				for (int path = 0; path < 13; path++) {
					Vector *vec = &m_vectors[ch][path];
					DBG(printf("vector[%d][%d]:\n", ch, path));
					/* get delayed samps */
					get_tap(cursamp, ch, path, bufsamps);
					DBG(PrintSig(vec->Sig, bufsamps));   
					/* air absorpt. filters */
         				air(vec->Sig, bufsamps, vec->Airdata);			
					/* wall absorpt. filters */
					if (path > 0)	// no filtering of direct signal
         				wall(vec->Sig, bufsamps, vec->Walldata);
					/* do binaural angle filters if necessary*/
					if (m_binaural)						
						fir(vec->Sig, cursamp, g_Nterms[path], 
					    	vec->Fircoeffs, vec->Firtaps, bufsamps);
           				// sum unscaled reflected paths as input for RVB.
					// first path is set; the rest are summed
					if (path == 1)
						copyBuf(&roomsig[ch][0], vec->Sig, bufsamps);             
					else if (path > 1)
						addBuf(&roomsig[ch][0], vec->Sig, bufsamps);             
					/* now do cardioid mike effect if not binaural mode */
					if (!m_binaural)
						scale(vec->Sig, bufsamps, vec->MikeAmp);
					DBG(printf("after final scale before rvb:\n"));
					DBG(PrintSig(vec->Sig, bufsamps, 0.1));
		 		}
				/* scale reverb input by amp factor */
				scale(&roomsig[ch][0], bufsamps, m_rvbamp);
			}
			/* run 1st and 2nd generation paths through reverberator */
 			for (int n = 0; n < bufsamps; n++) {
			    if (m_rvbamp != 0.0) {
			    	double rmPair[2];
					double rvbPair[2];
					rmPair[0] = roomsig[0][n];
					rmPair[1] = roomsig[1][n];
					RVB(rmPair, rvbPair, cursamp + n);
					rvbsig[0][n] = rvbPair[0];
					rvbsig[1][n] = rvbPair[1];
				}
				else
					rvbsig[0][n] = rvbsig[1][n] = 0.0;
			}
			DBG(printf("summing vectors\n"));
			if (!m_binaural) {
           		// re-sum scaled direct and reflected paths as early response
				// first path is set; the rest are summed
				for (int path = 0; path < 13; path++) {
					if (path == 0) {
						copyBuf(&roomsig[0][0], m_vectors[0][path].Sig, bufsamps);
						copyBuf(&roomsig[1][0], m_vectors[1][path].Sig, bufsamps);
					}           
					else {
						addBuf(&roomsig[0][0], m_vectors[0][path].Sig, bufsamps);
						addBuf(&roomsig[1][0], m_vectors[1][path].Sig, bufsamps);
					}
				}          
			}
			DBG(printf("left signal:\n"));
			DBG(PrintSig(roomsig[0], bufsamps, 0.1));
			DBG(printf("right signal:\n"));
			DBG(PrintSig(roomsig[1], bufsamps, 0.1));
			/* sum the early response & reverbed sigs  */
			register float *outptr = &this->outbuf[i*2];
			for (int n = 0; n < bufsamps; n++) {
				*outptr++ = roomsig[0][n] + rvbsig[0][n];
				*outptr++ = roomsig[1][n] + rvbsig[1][n];
			}
			DBG(printf("FINAL MIX:\n"));
			DBG(PrintInput(&this->outbuf[i], bufsamps));
		}

		else {         /* flushing reverb */
		    // this is the current location in the main output buffer
			// to write to now
			register float *outptr = &this->outbuf[i*2];
			DBG1(printf("  flushing reverb: i = %d, bufsamps = %d\n", i, bufsamps));
			for (int n = 0; n < bufsamps; n++) {
				if (m_rvbamp > 0.0) {
					double rmPair[2];
					double rvbPair[2];
					rmPair[0] = 0.0;
					rmPair[1] = 0.0;
					RVB(rmPair, rvbPair, cursamp + n);
					*outptr++ = rvbPair[0];
					*outptr++ = rvbPair[1];
				}
				else {
					*outptr++ = 0.0;
					*outptr++ = 0.0;
				}
			}
		}
		cursamp += bufsamps;
		i += bufsamps;
		bufsamps = getBufferSize();		// update
	}
	DBG1(printf("%s::run done\n\n", name()));
	return i;
}

void
BASE::wire_matrix(double Matrix[12][12])
{
	int i, j, n;
	int row, col;
		
	// assign output pointers
	for (i = 0, n = 0; i < 2; i++)
		for (j = 0; j < 6; j++) {
		    ReverbPatches[n].outptr = &m_rvbData[i][j].delin;
			ReverbPatches[n].incount = 0;
			n++;
		}

	for (row = 0; row < 12; row++) {
	    for (col = 0, n = 0; col < 12; col++) {
			if (Matrix[row][col] != 0.0) {
				i = col / 6;
				j = col % 6;
				ReverbPatches[row].inptrs[ReverbPatches[row].incount] = &m_rvbData[i][j].delout;
				ReverbPatches[row].gains[ReverbPatches[row].incount++] = Matrix[row][col];
			}
		}
	}
}

/* -------------------------------------------------------------------------- */
/* The following functions from the original space.c. */
/* -------------------------------------------------------------------------- */

void BASE::alloc_firfilters()
{
   /* allocate memory for FIR filters and zero delays */
   for (int i = 0; i < 2; i++) {
      for (int j = 0; j < 13; j++) {
         m_vectors[i][j].Firtaps = new double[g_Nterms[j] + 1];
		 memset(m_vectors[i][j].Firtaps, 0, (g_Nterms[j] + 1) * sizeof(double));
         m_vectors[i][j].Fircoeffs = new double[g_Nterms[j]];
      }
   }
}

/* --------------------------------------------------------- alloc_delays --- */
/* Sets aside the memory needed for the delays in RVB, and computes a
   length for the tap delay line (m_tapsize).
*/
void BASE::alloc_delays()
{
   double diag, maxdim, mindim, d1, d0;

   /* the length for the main tape delay */

   d0 = Dimensions[0] - Dimensions[2];
   d1 = Dimensions[1] - Dimensions[3];
   maxdim = (d0 > d1) ? d0 : d1;
   mindim = (d0 > d1) ? d1 : d0;
   diag = hypot((3 * maxdim), mindim);
   m_tapsize = (int)(diag / MACH1 * SR + 32);

   /* allocate memory for reverb delay lines */
   for (int i = 0; i < 2; i++) {
      for (int j = 0; j < 6; j++) {
         m_rvbData[i][j].Rvb_del = new double[rvbdelsize];	// zeroed in rvb_reset
      }
   }
}


/* ---------------------------------------------------------- get_lengths --- */
/* Computes the lengths for the 2x6 delays used in reverb, and determines
   the max number of elements needed for allocation.
*/
void BASE::get_lengths(long m_length)
{
   int i, j, max = 0;
   static float delfac[2][6] = {        /* These determine the distribution */
      {0.819, 0.918, 1.0, 1.11, 1.18, 1.279},
      {0.811, 0.933, 0.987, 1.13, 1.21, 1.247}
   };

   for (i = 0; i < 2; ++i) {
      for (j = 0; j < 6; ++j) {
         int val = (int)(m_length * delfac[i][j] + 0.5); /* 2x6 diff. lengths */
         int pval = close_prime(val, NPRIMES, primes);  /* all lengths primes */
         Nsdelay[i][j] = pval;

         /* the number of elements (max) for allocation */
         max = (pval > max) ? pval : max;
      }
   }
   /* extra to allow for varying taps dur to random tap */
   rvbdelsize = (int)(max * 1.2);
}


/* ------------------------------------------------------------ set_gains --- */
/* Determines the proper gains for the feedback filters in reverb, based on
   reverb time and average delay length. It also creates the global array of
   coefficients.
*/
void BASE::set_gains(float rvbtime)
{
   int    i, fpoint, nvals = 16;
   float  rescale, gain, dist, G1, temp = SR / MACH1;
   double adjust;
   static float array[16] = {
      1, .001, 10, .1, 25, .225, 35, .28, 50, .35, 65, .4, 85, .45, 95, .475
   };

   /* compensate for variable delay lengths */
   rescale = (0.05 * SR) / Nsdelay[0][0];

   /* compensate for SR differences */
   adjust = 1.0 - (0.42 * (SR - 25000) / 25000.0);

   /* create scaled curve for coeffs */
   setline(array, nvals, NCOEFFS, AIRCOEFFS);

   for (i = 0; i < NCOEFFS; i++)
      AIRCOEFFS[i] = pow((double)AIRCOEFFS[i], adjust);

   gain = 1.0 - 0.366 / (rvbtime * rescale);               /* a la Moorer */
   gain = (gain < 0.0) ? 0.0 : gain;
#ifdef debug
   printf("number of samples in each reverb delay line.");
#endif
   for (i = 0; i < 2; ++i) {
#ifdef debug
      printf("\nchannel %d:\n\n", i);
#endif
      for (int j = 0; j < 6; ++j) {
         dist = Nsdelay[i][j] / temp;
         fpoint = (int)(dist * (float)NCOEFFS / 300.0); /* 300 ft. max dist. */
         /* to avoid overrunning: */
         fpoint = (fpoint <= NCOEFFS - 1) ? fpoint : NCOEFFS - 1;
         G1 = AIRCOEFFS[fpoint - 1];                 /* G1 is filter coeff. */
         m_rvbData[i][j].Rvb_air[0] = gain * (1.0 - G1);       /* G2 is filt. gain */
         m_rvbData[i][j].Rvb_air[1] = G1;
#ifdef debug
         printf("delay %d: %g (%7.2f ms.) g1 = %f\n\n", j, Nsdelay[i][j],
                float(Nsdelay[i][j] * 1000.0 / SR), G1);
#endif
      }
   }
}


/* ------------------------------------------------------------ set_walls --- */
/* Initializes the tone filters for simulating wall absorption.
   <wallfac> is a value between 0 and 10.
*/
void BASE::set_walls(float wallfac)
{
   float cutoff, cf;

   wallfac /= 10.0;
   wallfac *= wallfac;                 /* now an expon. value between 0 and 1 */
   cutoff = wallfac * SR / 2;          /* sets -3db pt. between 0 & N.F. */
   cutoff = (cutoff <= SR / 2) ? cutoff : SR / 2;     /* set limit at N.F. */

   for (int i = 0; i < 2; i++) {
      for (int j = 1; j < 13; j++) {        /* skip first pair (direct sigs) */
         cf = (j > 4) ? cutoff * .6 : cutoff;   /* more filt for 2nd */
         toneset(cf, 1, m_vectors[i][j].Walldata);        /* gen. wall reflect */
      }
   }
}


/* ---------------------------------------------------------- set_allpass --- */
/* Initializes the allpass filters for reverb.
*/
void BASE::set_allpass()
{
   Allpass_del[0][0] = 0.72;         /* the gains */
   Allpass_del[1][0] = 0.72;
   Allpass_del[0][1] = 180.0;        /* the delay lengths */
   Allpass_del[1][1] = 183.0;
}


/* ----------------------------------------------------------- set_random --- */
/* Initializes the random functions used to vary the delay lengths
   in the reverb routine.
*/
void BASE::set_random()
{
   static const float rnd_freq[2][6] = {    /* the freq of rand variations */
      {4.9, 5.2, 6.7, 3.4, 2.7, 3.8},
      {5.1, 6.3, 7.0, 3.6, 2.8, 4.4}
   };
   static const float rnd_pcnt[2][6] = {    /* the amt. of delay to vary */
      {.0016, .0013, .0021, .0018, .0019, .0014},
      {.0027, .0014, .0020, .0016, .0012, .0015}
   };
   static const float seed[2][6] = {        /* the seeds for randi */
      {.314, .159, .265, .358, .979, .323},
      {.142, .857, .685, .246, .776, .456}
   };

   for (int i = 0; i < 2; i++) {              /* loop for 2 chans worth */
      for (int j = 0; j < 6; j++) {           /* loop for 6 delays per chan */
         ReverbData *r = &m_rvbData[i][j];
         r->Rand_info[0] = rnd_pcnt[i][j] * SR / 2.0;    /* nsamps jitter */
         r->Rand_info[1] = cycle(rnd_freq[i][j], 512);   /* SI  */
         r->Rand_info[2] = 1.0;
         r->Rand_info[4] = seed[i][j];     /* load seeds */
      }
   }
}

/* ----------------------------------------------------------- matrix_mix --- */
/* matrix_mix distributes the outputs from the delays (in reverb) back
   into the inputs according to the signal routing matrix set up by the
   matrix routine.
*/
void BASE::matrix_mix()
{
	for (int i = 0; i < 12; ++i) {
		ReverbPatch *patch = &ReverbPatches[i];
   		double *outptr = patch->outptr;
		*outptr = 0.0;
		for (int n = 0; n < patch->incount; n++) {
			*outptr += *patch->inptrs[n] * patch->gains[n];
		}
	}
}	        

/* -------------------------------------------------------------- delpipe --- */
/* delpipe is a delay with a variable interpolated tap, used by reverb. */

inline double
delpipe(double sig, int *counter, double nsdel, int delsize, double *delay)
{
   register int intap, tap1, tap2, del1;

   intap = *counter;
   if (intap >= delsize)
       intap -= delsize;
   delay[intap] = sig;                    /* put samp into delay */
   del1 = (int)nsdel;
   tap1 = intap - del1;
   if (tap1 < 0)
       tap1 += delsize;
   tap2 = tap1 - 1;
   if (tap2 < 0)
       tap2 += delsize;
   
   *counter = ++intap;			// increment and store
  
   double frac = nsdel - del1;
   return (delay[tap1] + (delay[tap2] - delay[tap1]) * frac);
}

/* -------------------------------------------------------------- Allpass --- */
/* Allpass is the allpass filter used by reverb */
inline double
Allpass(double sig, int *counter, double *data)
{
   register int nsdel, intap, length, outtap;
   double gsig;
   double *delay = &data[2];

   nsdel = (int)data[1];
   length = nsdel + 1;
   intap = *counter;
   if (intap >= length)
       intap -= length;
   outtap = intap - nsdel;
   if (outtap < 0)
      outtap += length;

   /* Input = sig + gain * (sig - delayed out)  */

   gsig = data[0] * (sig - delay[outtap]);
   delay[intap] = sig + gsig;

   *counter = ++intap;	// increment and store
   
   /* Output = delayed out + gain * (sig - delayed out)  */
   return (delay[outtap] + gsig);
}

// recreate buggy code which produces decent reverb -- correct code does not!
#define USE_BUGGY_CODE

/* ------------------------------------------------------------------ RVB --- */
/* This is the main routine, RVB. It is a bank of 2x6 randomly varying
   delays which feed back into themselves in a tricky way.
*/
void BASE::RVB(double *input, double *output, long counter)
{
   register int i, j;
   double sig, delsig;

   for (i = 0; i < 2; ++i) {                /* loop for 2 output chans */
      output[i] = 0.0;
      for (j = 0; j < 6; ++j) {             /* loop for 6 delays per chan */
		 ReverbData *rvb = &m_rvbData[i][j];
	 	 ReverbData *rvb2 = rvb;
	 
         sig = input[i] + rvb->delin;      /* combine input w/ delay return */

         /* get new delay length (random) then 
            put samp into delay & get new delayed samp out
         */
         double delay = Nsdelay[i][j] + randi(&rvb->Rand_info[0]);
         delsig = delpipe(sig, &rvb->deltap, delay, rvbdelsize, &rvb->Rvb_del[0]);

#ifdef USE_BUGGY_CODE
         if (i == 1 && j > 0)
	     rvb2 = &m_rvbData[0][j];	// remap "incorrectly"
#endif
         /* filter with air simulation filters, set gains */
         rvb->delout = tone(delsig, &rvb2->Rvb_air[0]);

         /* sum outputs of all delays for 2 channels */
         output[i] += rvb->delout;
      }
      /* run outputs through Allpass filter */
      output[i] = Allpass(output[i], &allpassTap[i], Allpass_del[i]);
   }

   /* redistribute delpipe outs into delpipe ins */
   matrix_mix();
}



/* -------------------------------------------------------------------------- */
/* The following functions from the original placelib.c. */
/* -------------------------------------------------------------------------- */


/* ------------------------------------------------------------- roomtrig --- */
/* roomtrig calculates all distance/angle vectors for images up through the
   2nd generation reflections. 13 vectors total:  1 source, 4 1st gen., 8
   2nd gen. These are split into stereo pairs by binaural.
*/
int BASE::roomtrig(double A,                 /* 'rho' or 'x' */
                    double B,                 /* 'theta' or 'y' */
                    double H,
                    int    cart)
{
   register int i;
   double x[13], y[13], r[13], t[13], d[4], Ra[2], Ta[2];
   double X, Y, R, T;
   const double z = 0.017453292;  /* Pi / 180 */

   /* calc. X & Y if entered in polar form only */

   if (!cart) {                 /* polar coordinates */
      R = A;
      T = B;	/* already in radians */
      X = A * sin(T);
      Y = A * cos(T);
   }
   else {
      R = hypot(A, B);
      T = atan(A / B);
      if (B < 0.0)
         T += M_PI;
      X = A;
      Y = B;
   }

   /* Check to see that source loc. is inside room bounds */

   if (X < Dimensions[3] || X > Dimensions[1] || Y > Dimensions[0] ||
       Y < Dimensions[2]) {
      fprintf(stderr, "Source loc outside room bounds!!\n\n");
      return (1);
   }

   /* multiply global dimension array by 2 to save calc. time */

   for (i = 0; i < 4; ++i)
      d[i] = Dimensions[i] * 2.0;

   /* image calculations */

   /* source vector */
   x[0] = X;
   y[0] = Y;
   t[0] = T;
   r[0] = R;

   /* front wall vector */
   x[1] = X;
   y[1] = d[0] - Y;
   t[1] = atan(x[1] / y[1]);
   r[1] = y[1] / cos(t[1]);

   /* right wall vector */
   x[2] = d[1] - X;
   y[2] = Y;
   t[2] = PI / 2.0 - atan(y[2] / x[2]);
   r[2] = x[2] / sin(t[2]);

   /* back wall vector */
   x[3] = X;
   y[3] = d[2] - Y;
   t[3] = PI + atan(x[3] / y[3]);
   r[3] = y[3] / cos(t[3]);

   /* left wall vector */
   x[4] = d[3] - X;
   y[4] = Y;
   t[4] = 3. * PI / 2. - atan(y[4] / x[4]);
   r[4] = x[4] / sin(t[4]);

   /* 2nd gen. images: 4 opposing wall, 4 adjacent wall reflections */

   /* front wall vector */
   x[5] = X;
   y[5] = d[0] - d[2] + Y;
   t[5] = atan(x[5] / y[5]);
   r[5] = hypot(X, y[5]);

   /* right wall vector */
   x[6] = d[1] - d[3] + X;
   y[6] = Y;
   t[6] = PI / 2.0 - atan(y[6] / x[6]);
   r[6] = hypot(x[6], Y);

   /* back wall vector */
   x[7] = X;
   y[7] = d[2] - d[0] + Y;
   t[7] = PI + atan(x[7] / y[7]);
   r[7] = hypot(X, y[7]);

   /* left wall vector */
   x[8] = d[3] - d[1] + X;
   y[8] = Y;
   t[8] = 3.0 * PI / 2.0 - atan(y[8] / x[8]);
   r[8] = hypot(x[8], Y);

   /* fr. rt. vector - double image in rectangular room, as are next 3 */
   x[9] = x[2];
   y[9] = y[1];
   t[9] = atan(x[9] / y[9]);
   r[9] = hypot(x[9], y[9]);

   /* back rt. vector */
   x[10] = x[2];
   y[10] = y[3];
   t[10] = PI / 2.0 - atan(y[10] / x[10]);
   r[10] = hypot(x[10], y[10]);

   /* back lft. vector */
   x[11] = x[4];
   y[11] = y[3];
   t[11] = PI + atan(x[11] / y[11]);
   r[11] = hypot(x[11], y[11]);

   /* front lft. vector */
   x[12] = x[4];
   y[12] = y[1];
   t[12] = 3.0 * PI / 2.0 - atan(y[12] / x[12]);
   r[12] = hypot(x[12], y[12]);

   /* calculate stereo vector pairs for each of these */
   for (i = 0; i < 13; ++i) {
      binaural(r[i], t[i], x[i], y[i], H, Ra, Ta);
	  register Vector *lvec = &m_vectors[0][i];
	  register Vector *rvec = &m_vectors[1][i];
      lvec->Rho = Ra[0];
      rvec->Rho = Ra[1];
      lvec->Theta = Ta[0];
      rvec->Theta = Ta[1];
   }

   /* Check to see that source distance is not "zero" */

   if (m_vectors[0][0].Rho < 0.001 || m_vectors[1][0].Rho < 0.001) {
      fprintf(stderr, "Zero source distance not allowed!\n\n");
      return (1);
   }

   /* print out data for analysis */
#ifdef debug
   printf("Source angles: %.2f    %.2f\n", m_vectors[0][0].Theta / z, m_vectors[1][0].Theta / z);
   printf("All others\n");
   for (i = 1; i < 13; i++)
      printf("%.2f     %.2f\n", m_vectors[0][i].Theta / z, m_vectors[1][i].Theta / z);
   printf("Direct delays:\n");
   printf("%d: %.2f ms.       %.2f ms.\n", i, m_vectors[0][0].Rho / 1.08, m_vectors[1][0].Rho / 1.08);
   printf("Room delays:\n");
   for (i = 1; i < 13; i++)
      printf("%.2f ms.       %.2f ms.\n ", m_vectors[0][i].Rho / 1.08, m_vectors[1][i].Rho / 1.08);
   printf("\n");
   printf("Direct dists:\n");
   printf("%d: %.2f ft.       %.2f ft.\n", i, m_vectors[0][0].Rho, m_vectors[1][0].Rho);
   printf("Room dists:\n");
   for (i = 1; i < 13; i++)
      printf("%.2f ft.       %.2f ft.\n ", m_vectors[0][i].Rho, m_vectors[1][i].Rho);
   printf("\n");
#endif

   return 0;
}


/* ------------------------------------------------------------ rvb_reset --- */
/* reset zeroes out all delay and filter histories in move and reverb
   each time move is called
*/
void BASE::rvb_reset(double *m_tapDelay)
{
	register int i, j, k;

	/* reset wall filter hists */

	for (i = 0; i < 2; ++i) {
		for (j = 0; j < 13; ++j)
			m_vectors[i][j].Walldata[2] = 0.0;
	}

	/* reset reverb filters and delays */

	for (i = 0; i < 2; ++i) {
		for (j = 0, k = 0; j < 6; ++j) {
			ReverbData *r = &m_rvbData[i][j];
			r->delin = r->delout = 0.0;
			r->deltap = 0;	// index for each delay unit
			r->Rvb_air[2] = 0.0;
			register double *point = r->Rvb_del;
			while (k < rvbdelsize)
				point[k++] = 0.0;
		}
		for (j = 2; j < 502; ++j)
			Allpass_del[i][j] = 0.0;
		allpassTap[i] = 0; 	// indices for allpass delay
	}

	/* reset tap delay for move */

	for (i = 0; i < m_tapsize + 8; ++i)
		m_tapDelay[i] = 0.0;
}


/* --------------------------------------------------------------- setair --- */
/* setair calculates and loads the gain factors (G1 & G2) for the tone
   filters used to simulate air absorption. The values for G1 are stored
   in AIRCOEFFS by space(). G2 takes the 1/r-squared attenuation factor into
   account.  Histories are reset to 0 if flag = 1.
*/
void BASE::setair(double rho, int flag, double *coeffs)
{
   int    gpoint;
   float  fpoint, frac;
   double G1, G2;

   /* pointer into array. Max distance for rho is 300 ft. */

   rho = (rho > 300 ? 300 : rho);
   fpoint = rho * 511 / 300.0;
   gpoint = (int)fpoint;
   frac = fpoint - (float)gpoint;
   G1 = AIRCOEFFS[gpoint] + frac * (AIRCOEFFS[gpoint + 1] - AIRCOEFFS[gpoint]);
   G2 = (1.0 / rho) * (1.0 - G1);

   /* load into output array */

   coeffs[0] = G2;
   coeffs[1] = G1;

   /* reset if flag = 1 */

   if (flag)
      coeffs[2] = 0.0;
}


/* ----------------------------------------------------------- airfil_set --- */
/* airfil_set is called by move to set all airfilters (2 for each image
   pair) whenever source moves.
*/
void BASE::airfil_set(int flag)
{
   for (int i = 0; i < 2; ++i)
      for (int j = 0; j < 13; ++j)
         setair(m_vectors[i][j].Rho, flag, m_vectors[i][j].Airdata);
}

/* -------------------------------------------------------------- put_tap --- */
/* Accesses the tap delay array and loads the signal buffer into it. */

void BASE::put_tap(int intap, float *Sig, int len)
{
	register double *tapdel = m_tapDelay;
	int tap = intap;
    while (tap >= m_tapsize)
        tap -= m_tapsize;
	for (int i = 0; i < len; ++i, tap++) {
	    if (tap >= m_tapsize)
		    tap = 0;
    	tapdel[tap] = Sig[i];
	}
}

/* ------------------------------------------------------------- mike_set --- */
/* mike_set determines the gain of each source vector based on its angle to
   the simulated microphones
 */
void BASE::mike_set()
{
   double OmniFactor = 1.0 - MikePatternFactor;

   for (int i = 0; i < 2; ++i)
      for (int j = 0; j < 13; ++j)
         m_vectors[i][j].MikeAmp = 
		     OmniFactor + (MikePatternFactor * cos(m_vectors[i][j].Theta - MikeAngle));
}

/* ----------------------------------------------------------- earfil_set --- */
/* earfil_set is called by place to load coeffs for the fir filter bank
   used in ear, the binaural image filter.
*/
void
BASE::earfil_set(int flag)
{
   for (int i = 0; i < 2; ++i)
      for (int j = 0; j < 13; ++j)
         setfir(m_vectors[i][j].Theta,
		        g_Nterms[j],
				flag,
				m_vectors[i][j].Fircoeffs,
				m_vectors[i][j].Firtaps);
}


/* -------------------------------------------------------------- tap_set --- */
/* tap_set determines the number of samps delay for each of the tap delay
   lines in move.  It compensates for the group phase delay in ear, and
   determines the max # of samps delay for loop purposes, later in move.
*/
long
BASE::tap_set(int fir_flag)
{
   long int maxloc = 0;
   double   delay;

   extern int g_Group_delay[13];        /* from firdata.h */

#ifdef debug
   printf("outlocs:\n");
#endif
   for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 13; ++j) {
         delay = m_vectors[i][j].Rho / MACH1;                /* delay time */
         if (fir_flag)
            m_vectors[i][j].outloc = (long)(delay * SR - g_Group_delay[j] + 0.5);
         else
            m_vectors[i][j].outloc = (long)(delay * SR + 0.5);
         if (m_vectors[i][j].outloc > (float)maxloc)
            maxloc = (int)(m_vectors[i][j].outloc + 0.5);   /* max # of samps delay */
#ifdef debug
         printf("%ld ", m_vectors[i][j].outloc);
#endif
      }
#ifdef debug
      printf("\n");
#endif

   }
#ifdef debug
   printf("\n");
#endif
   return (maxloc);
}

/* ----------------------------------------------------------- get_primes --- */
/* Loads <p> array with up to <x> prime numbers for use in determining
   prime delay lengths.
*/
void
BASE::get_primes(int x, int p[])
{
   int val = 5, flag, i, index = 2;

   if (!primes_gotten)
   {
	/* first 2 vals initialized */
	p[0] = 2;
	p[1] = 3;

	while (index < x) {
	   flag = 1;
	   for (i = 1; flag && val / p[i] >= p[i]; ++i)
              if (val % p[i] == 0)
        	 flag = 0;
	   if (flag) {
              p[index] = val;
              ++index;
	   }
	   val += 2;
	}
	primes_gotten = 1;
    }
}
