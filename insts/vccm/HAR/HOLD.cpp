#include <unistd.h>
#include <stdio.h>
#include <ugens.h>
#include <mixerr.h>
#include <Instrument.h>
#include <globals.h>
#include <rt.h>
#include <rtdefs.h>
#include "HOLD.h"

extern float *temp_buff[];
extern int samp_marker;
extern int hold_samps;
extern int hold_start;
extern float hold_dur;

HOLD::HOLD() : Instrument()
{
	in = NULL;
}

HOLD::~HOLD()
{
	delete [] in;
}

int HOLD::init(float p[], int n_args)
{
// p0 = outsk; 
// p1 = duration (-endtime); 
// p2 = amp; 
// p3 = aud_idx; 
// p4 = fade time; 
// p5 = spread
// setline info is in gen table 1

	int i;
	float fade_time;

	if (p[1] < 0.0) p[1] = -p[1] - p[0];

	nsamps = rtsetoutput(p[0], p[1], this);
	/* rtsetinput(p[1], this); */

	dur = p[1];
	amp = p[2];
	aud_idx = (int)p[3];
	fade_time = p[4];
	fade_samps = (int)(fade_time*SR);
	fade_samp = 0;
	spread = p[5];
	hold_samp = 0;
	hold_dur = (int)(hold_samps/SR);
	idx_samp = 0;

	start_fade = NO;

	if (outputchans > 2) {
			die("HOLD", "This is a MONO output instrument.  Reset your bus_config to use only 1 output bus\n");
	}

	amptable = floc(1);
	if (amptable) {
		int amplen = fsize(1);
		tableset(hold_dur, amplen, tabs);
	}
	else
		advise("HOLD", "Setting phrase curve to all 1's for *loop* duration.");

	if (fade_time) {
	  	fade_table = floc(2);
		if (fade_table) {
		   	int famplen = fsize(2);
			tableset(fade_time, famplen, f_tabs);
		}
	}	

	skip = (int)(SR/(float)resetval);

	return(nsamps);
}

int HOLD::run()
{
	int i,j,k,rsamps,finalsamp;
	float out[MAXBUS];
	float aamp,famp;
	int branch;

	aamp = famp = 0.0;
	if (!start_fade) 
		stop_hold = YES;

	if (in == NULL) {                 /* first time, so allocate it */
		in = new float [RTBUFSAMPS];
		idx_samp += samp_marker+1;
	}

	Instrument::run();

	rsamps = chunksamps;

	branch = 0;
	for (i = 0; i < rsamps; i++)  {

	  if (idx_samp > hold_samps) 
		idx_samp = 0;
	  if (hold_samp > hold_samps)
		hold_samp = 0;

	  if (--branch < 0) {
		if (amptable)
		  aamp = tablei(hold_samp, amptable, tabs) * amp;
		else
		  aamp = amp;
		if (start_fade) {
		  if (!fade_started) {
 			finalsamp = i_chunkstart+i+fade_samps;
			this->setendsamp(finalsamp);
		  }
		  fade_started = YES;
		  if (fade_table)
			  famp = tablei(fade_samp, fade_table, f_tabs) * aamp;
		  else
			  famp = aamp;
		}
		branch = skip;
	  }
	  
	  if (!start_fade)
		  in[i] = temp_buff[aud_idx][idx_samp] * aamp;
	  else 
		  in[i] = temp_buff[aud_idx][idx_samp] * famp;
	
	  out[0] = in[i];
	  if (outputchans == 2) { /* split stereo files between the channels */
		out[1] = (1.0 - spread) * out[0];
		out[0] *= spread;
	  }
	  else {
		out[1] = 0.0;
	  }
	  for (j = 2; j < outputchans; j++) {
		out[j] = 0.0;
	  }

	  rtaddout(out);
	  cursamp++;
	  idx_samp++;
	  hold_samp++;
	  if (start_fade)
		fade_samp++;
	}
	return i;
}

Instrument*
makeHOLD()
{
	HOLD *inst;

	inst = new HOLD();
	inst->set_bus_config("HOLD");
	return inst;
}


