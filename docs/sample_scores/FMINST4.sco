/* FMINST -- simple fm instrument
*
*  p0 = start time
*  p1 = duration
*  p2 = amp
*  p3 = pitch of carrier (hz or oct.pc)
*  p4 = pitch of modulator (hz or oct.pc)
*  p5 = fm index low point
*  p6 = fm index high point
*  p7 = stereo spread (0-1) <optional>
*  function slot 1 is oscillator waveform, slot 2 is the amp envelope,
*     slot 3 is index guide
*/

rtsetparams(44100, 2)
load("FMINST")
/* print_off() */
makegen(1, 10, 1000, 1)
makegen(2, 7, 1000, 0, 500, 1, 500, 0)
makegen(3, 24, 1000, 0,1, 2,0)

freq = 8.00
for (start = 0; start < 60; start = start + 0.1) {
	FMINST(start, .5, 1000, freq, 179, 0, 10, random())
	freq = freq + 0.002
}
