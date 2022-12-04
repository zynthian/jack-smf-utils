/*-
 * Copyright (c) 2007, 2008 Edward Tomasz Napierała <trasz@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * ALTHOUGH THIS SOFTWARE IS MADE OF SCIENCE AND WIN, IT IS PROVIDED BY THE
 * AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * This is jack-smf-player, Standard MIDI File player for JACK MIDI.
 *
 * For questions and comments, contact Edward Tomasz Napierala <trasz@FreeBSD.org>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <sysexits.h>
#include <errno.h>
#include <signal.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <glib.h>

#include "config.h"
#include "smf.h"

#ifdef WITH_LASH
#include <lash/lash.h>
#endif

#define PROGRAM_NAME		"jack-smf-player"
#define PROGRAM_VERSION		PACKAGE_VERSION

#define MIDI_CONTROLLER         0xB0
#define MIDI_ALL_SOUND_OFF      120

#define MAX_NUMBER_OF_TRACKS	128

jack_port_t *output_ports[MAX_NUMBER_OF_TRACKS];
jack_client_t *jack_client = NULL;
smf_t *smf = NULL;

int drop_messages = 0;
double rate_limit = 0;
int just_one_output = 0;
int start_stopped = 0;
int use_transport = 1;
int loop_forever = 0;
int be_quiet = 0;
int ctrl_c_pressed = 0;
double playback_started = -1;
double song_position = 0;
double song_bpm = 120.0;
double current_bpm = 120.0;
double bpm_ratio = 1.0;
double last_frame_song_tsecs = 0;

#ifdef WITH_LASH
lash_client_t	*lash_client;
#endif

/* Will emit a warning if time between jack callbacks is longer than this. */
#define MAX_TIME_BETWEEN_CALLBACKS	0.1

/* Will emit a warning if execution of jack callback takes longer than this. */
#define MAX_PROCESSING_TIME	0.01

double 
get_time(void)
{
	double seconds;
	int ret;
	struct timeval tv;

	ret = gettimeofday(&tv, NULL);

	if (ret) {
		perror("gettimeofday");
		exit(EX_OSERR);
	}

	seconds = tv.tv_sec + tv.tv_usec / 1000000.0;

	return seconds;
}

double
get_delta_time(void)
{
	static double previously = -1.0;
	double now;
	double delta;

	now = get_time();

	if (previously == -1.0) {
		previously = now;

		return 0;
	}

	delta = now - previously;
	previously = now;

	assert(delta >= 0.0);

	return delta;
}

static gboolean
warning_async(gpointer s)
{
	const char *str = (const char *)s;

	g_warning(str);

	return FALSE;
}

static void
warn_from_jack_thread_context(const char *str)
{
	g_idle_add(warning_async, (gpointer)str);
}

static double
nframes_to_ms(jack_nframes_t nframes)
{
	jack_nframes_t sr;

	sr = jack_get_sample_rate(jack_client);

	assert(sr > 0);

	return (nframes * 1000.0) / (double)sr;
}

static double
nframes_to_seconds(jack_nframes_t nframes)
{
	jack_nframes_t sr;

	sr = jack_get_sample_rate(jack_client);

	assert(sr > 0);

	return nframes / (double)sr;
}

static jack_nframes_t
ms_to_nframes(double ms)
{
	jack_nframes_t sr;

	sr = jack_get_sample_rate(jack_client);

	assert(sr > 0);

	return ((double)sr * ms) / 1000.0;
}

static jack_nframes_t
seconds_to_nframes(double seconds)
{
	jack_nframes_t sr;

	sr = jack_get_sample_rate(jack_client);

	assert(sr > 0);

	return (double)sr * seconds;
}

static void
send_all_sound_off(void *port_buffers[MAX_NUMBER_OF_TRACKS], jack_nframes_t nframes)
{
	int i, channel;
	unsigned char *buffer;

	for (i = 0; i <= smf->number_of_tracks; i++) {
		for (channel = 0; channel < 16; channel++) {
#ifdef JACK_MIDI_NEEDS_NFRAMES
			buffer = jack_midi_event_reserve(port_buffers[i], 0, 3, nframes);
#else
			buffer = jack_midi_event_reserve(port_buffers[i], 0, 3);
#endif
			if (buffer == NULL) {
				warn_from_jack_thread_context("jack_midi_event_reserve failed, cannot send All Sound Off.");
				break;
			}

			buffer[0] = MIDI_CONTROLLER | channel;
			buffer[1] = MIDI_ALL_SOUND_OFF;
			buffer[2] = 0;
		}

		if (just_one_output)
			break;
	}
}

void
update_bpm(jack_position_t *position)
{
	if (position->beats_per_minute>0.0 && position->beats_per_minute!=current_bpm) {
		current_bpm = position->beats_per_minute;
		bpm_ratio = current_bpm/song_bpm;
		g_message("CURRENT BPM: %f",current_bpm);
	}
}

static void
process_midi_output(jack_nframes_t nframes)
{
	int i, t, bytes_remaining, track_number;
	unsigned char *buffer, tmp_status;
	void *port_buffers[MAX_NUMBER_OF_TRACKS];
	//jack_nframes_t last_frame_time;
	jack_transport_state_t transport_state;
	static jack_transport_state_t previous_transport_state = JackTransportStopped;
	jack_position_t transport_pos;

	for (i = 0; i <= smf->number_of_tracks; i++) {
		port_buffers[i] = jack_port_get_buffer(output_ports[i], nframes);

		if (port_buffers[i] == NULL) {
			warn_from_jack_thread_context("jack_port_get_buffer failed, cannot send anything.");
			return;
		}

#ifdef JACK_MIDI_NEEDS_NFRAMES
		jack_midi_clear_buffer(port_buffers[i], nframes);
#else
		jack_midi_clear_buffer(port_buffers[i]);
#endif

		if (just_one_output)
			break;
	}

	if (ctrl_c_pressed) {
		send_all_sound_off(port_buffers, nframes);
		
		/* The idea here is to exit at the second time process_midi_output gets called.
		   Otherwise, All Sound Off won't be delivered. */
		ctrl_c_pressed++;
		if (ctrl_c_pressed >= 3)
			exit(0);

		return;
	}

	if (use_transport) {
		transport_state = jack_transport_query(jack_client, &transport_pos);
		if (transport_state == JackTransportStopped) {
			if (previous_transport_state == JackTransportRolling)
				send_all_sound_off(port_buffers, nframes);
			previous_transport_state = transport_state;
			return;
		}
		previous_transport_state = transport_state;
		update_bpm(&transport_pos);
	}

	//last_frame_time = jack_last_frame_time(jack_client);

	/* End of song already? */
	if (playback_started < 0)
		return;

	/* We may push at most one byte per 0.32ms to stay below 31.25 Kbaud limit. */
	bytes_remaining = nframes_to_ms(nframes) * rate_limit;

	for (;;) {
		smf_event_t *event = smf_peek_next_event(smf);

		if (event == NULL) {
			if (loop_forever) {
				song_position = 0;
				last_frame_song_tsecs = 0;
				smf_seek_to_seconds(smf, last_frame_song_tsecs);
				playback_started = jack_frame_time(jack_client);
			}
			else {
				if (!be_quiet)
					g_debug("End of song.");

				playback_started = -1;

				if (!use_transport)
					ctrl_c_pressed = 1;
			}
			break;
		}

		/* Process metadata events. */
		if (smf_event_is_metadata(event)) {
			char *decoded = smf_event_decode(event);
			if (decoded && !be_quiet)
				g_debug("Metadata: %s", decoded);

			/* Capture BPM setting */
			if (event->midi_buffer[1]==0x51) {
				int mspqn = (event->midi_buffer[3] << 16) + (event->midi_buffer[4] << 8) + event->midi_buffer[5];
				//Set Song Tempo (tempo reference)
				song_bpm = 60000000.0 / (double)mspqn;
				update_bpm(&transport_pos);
				g_message("SONG BPM: %f",song_bpm);
			}

			smf_get_next_event(smf);
			continue;
		}

		bytes_remaining -= event->midi_buffer_length;

		if (rate_limit > 0.0 && bytes_remaining <= 0) {
			warn_from_jack_thread_context("Rate limiting in effect.");
			break;
		}

		t = seconds_to_nframes((event->time_seconds-last_frame_song_tsecs)/bpm_ratio);
		//g_debug("PosInfo: %fs - %fs => %d (%f BPM)", event->time_seconds,last_frame_song_tsecs,t,current_bpm);

		/* If computed time is too much into the future, we'll need
		   to send it later. */
		if (t >= (int)nframes)
			break;

		/* If computed time is < 0, we missed a cycle because of xrun. */
		if (t < 0)
			t = 0;

		assert(event->track->track_number >= 0 && event->track->track_number <= MAX_NUMBER_OF_TRACKS);

		/* We will send this event; remove it from the queue. */
		smf_get_next_event(smf);

		/* First, send it via midi_out. */
		track_number = 0;

#ifdef JACK_MIDI_NEEDS_NFRAMES
		buffer = jack_midi_event_reserve(port_buffers[track_number], t, event->midi_buffer_length, nframes);
#else
		buffer = jack_midi_event_reserve(port_buffers[track_number], t, event->midi_buffer_length);
#endif

		if (buffer == NULL) {
			warn_from_jack_thread_context("jack_midi_event_reserve failed, NOTE LOST.");
			break;
		}

		memcpy(buffer, event->midi_buffer, event->midi_buffer_length);

		/* Ignore per-track outputs? */
		if (just_one_output)
			continue;

		/* Send it via proper output port. */
		track_number = event->track->track_number;

#ifdef JACK_MIDI_NEEDS_NFRAMES
		buffer = jack_midi_event_reserve(port_buffers[track_number], t, event->midi_buffer_length, nframes);
#else
		buffer = jack_midi_event_reserve(port_buffers[track_number], t, event->midi_buffer_length);
#endif

		if (buffer == NULL) {
			warn_from_jack_thread_context("jack_midi_event_reserve failed, NOTE LOST.");
			break;
		}

		/* Before sending, reset channel to 0. XXX: Not very pretty. */
		assert(event->midi_buffer_length >= 1);

		tmp_status = event->midi_buffer[0];

		if (event->midi_buffer[0] >= 0x80 && event->midi_buffer[0] <= 0xEF)
			event->midi_buffer[0] &= 0xF0;

		memcpy(buffer, event->midi_buffer, event->midi_buffer_length);

		event->midi_buffer[0] = tmp_status;
	}

	last_frame_song_tsecs += nframes_to_seconds(nframes)*bpm_ratio;
}

static int 
process_callback(jack_nframes_t nframes, void *notused)
{
#ifdef MEASURE_TIME
	if (get_delta_time() > MAX_TIME_BETWEEN_CALLBACKS) {
		warn_from_jack_thread_context("Had to wait too long for JACK callback; scheduling problem?");
	}
#endif

	/* Check for impossible condition that actually happened to me, caused by some problem between jackd and OSS4. */
	if (nframes <= 0) {
		warn_from_jack_thread_context("Process callback called with nframes = 0; bug in JACK?");
		return 0;
	}

	process_midi_output(nframes);

#ifdef MEASURE_TIME
	if (get_delta_time() > MAX_PROCESSING_TIME) {
		warn_from_jack_thread_context("Processing took too long; scheduling problem?");
	}
#endif

	return 0;
}

static int
sync_callback(jack_transport_state_t state, jack_position_t *position, void *notused)
{
	assert(jack_client);

	if (state == JackTransportStarting) {
		update_bpm(position);
		song_position = position->frame;
		last_frame_song_tsecs = nframes_to_seconds(position->frame)*bpm_ratio;
		smf_seek_to_seconds(smf, last_frame_song_tsecs);

		if (!be_quiet)
			g_debug("Seeking to %f seconds.", last_frame_song_tsecs);

		playback_started = jack_frame_time(jack_client);

	} else if (state == JackTransportStopped) {
		playback_started = -1;
	}

	return TRUE;
}

void timebase_callback(jack_transport_state_t state, jack_nframes_t nframes, jack_position_t *pos, int new_pos, void *notused)
{
	double min;			/* Minutes since frame 0. */
	long abs_tick;		/* Ticks since frame 0. */
	long abs_beat;		/* Beats since frame 0. */
	smf_tempo_t *tempo;
	static smf_tempo_t *previous_tempo = NULL;

	smf_event_t *event = smf_peek_next_event(smf);
	if (event == NULL)
		return;

	tempo = smf_get_tempo_by_pulses(smf, event->time_pulses);

	assert(tempo);

	if (new_pos || previous_tempo != tempo) {
		pos->valid = JackPositionBBT;
		pos->beats_per_bar = tempo->numerator;
		pos->beat_type = 1.0 / (double)tempo->denominator;
		pos->ticks_per_beat = event->track->smf->ppqn; /* XXX: Is this right? */
		pos->beats_per_minute = 60000000.0 / (double)tempo->microseconds_per_quarter_note;

		min = pos->frame / ((double) pos->frame_rate * 60.0);
		abs_tick = min * pos->beats_per_minute * pos->ticks_per_beat;
		abs_beat = abs_tick / pos->ticks_per_beat;

		pos->bar = abs_beat / pos->beats_per_bar;
		pos->beat = abs_beat - (pos->bar * pos->beats_per_bar) + 1;
		pos->tick = abs_tick - (abs_beat * pos->ticks_per_beat);
		pos->bar_start_tick = pos->bar * pos->beats_per_bar * pos->ticks_per_beat;
		pos->bar++; /* adjust start to bar 1 */

		previous_tempo = tempo;

	} else {
		/* Compute BBT info based on previous period. */
		pos->tick += nframes * pos->ticks_per_beat * pos->beats_per_minute / (pos->frame_rate * 60);

		while (pos->tick >= pos->ticks_per_beat) {
			pos->tick -= pos->ticks_per_beat;
			if (++pos->beat > pos->beats_per_bar) {
				pos->beat = 1;
				++pos->bar;
				pos->bar_start_tick += pos->beats_per_bar * pos->ticks_per_beat;
			}
		}
	}
}

/* Connects to the specified input port, disconnecting already connected ports. */
int
connect_to_input_port(const char *port)
{
	int ret;

	ret = jack_port_disconnect(jack_client, output_ports[0]);

	if (ret) {
		g_warning("Cannot disconnect MIDI port.");

		return -3;
	}

	ret = jack_connect(jack_client, jack_port_name(output_ports[0]), port);

	if (ret) {
		g_warning("Cannot connect to %s.", port);

		return -4;
	}

	g_warning("Connected to %s.", port);

	return 0;
}

static void 
init_jack(void)
{
	int i, err;

#ifdef WITH_LASH
	lash_event_t *event;
#endif

	jack_client = jack_client_open(PROGRAM_NAME, JackNullOption, NULL);

	if (jack_client == NULL) {
		g_critical("Could not connect to the JACK server; run jackd first?");
		exit(EX_UNAVAILABLE);
	}

#ifdef WITH_LASH
	event = lash_event_new_with_type(LASH_Client_Name);
	assert (event); /* Documentation does not say anything about return value. */
	lash_event_set_string(event, jack_get_client_name(jack_client));
	lash_send_event(lash_client, event);

	lash_jack_client_name(lash_client, jack_get_client_name(jack_client));
#endif

	err = jack_set_process_callback(jack_client, process_callback, 0);
	if (err) {
		g_critical("Could not register JACK process callback.");
		exit(EX_UNAVAILABLE);
	}

	if (use_transport) {
		err = jack_set_sync_callback(jack_client, sync_callback, 0);
		if (err) {
			g_critical("Could not register JACK sync callback.");
			exit(EX_UNAVAILABLE);
		}
#if 0
		err = jack_set_timebase_callback(jack_client, 1, timebase_callback, 0);
		if (err) {
			g_critical("Could not register JACK timebase callback.");
			exit(EX_UNAVAILABLE);
		}
#endif
	}

	assert(smf->number_of_tracks >= 1);

	/* We are allocating number_of_tracks + 1 output ports. */
	for (i = 0; i <= smf->number_of_tracks; i++) {
		char port_name[32];

		if (i == 0)
			snprintf(port_name, sizeof(port_name), "midi_out");
		else
			snprintf(port_name, sizeof(port_name), "track_%d_midi_out", i);

		output_ports[i] = jack_port_register(jack_client, port_name, JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

		if (output_ports[i] == NULL) {
			g_critical("Could not register JACK output port '%s'.", port_name);
			exit(EX_UNAVAILABLE);
		}

		if (just_one_output)
			break;
	}

	if (jack_activate(jack_client)) {
		g_critical("Cannot activate JACK client.");
		exit(EX_UNAVAILABLE);
	}
}

#ifdef WITH_LASH

static gboolean
lash_callback(gpointer notused)
{
	lash_event_t	*event;

	while ((event = lash_get_event(lash_client))) {
		switch (lash_event_get_type(event)) {
			case LASH_Restore_Data_Set:
			case LASH_Save_Data_Set:
				break;

			case LASH_Quit:
				g_warning("Exiting due to LASH request.");
				ctrl_c_pressed = 1;
				break;

			default:
				g_warning("Receieved unknown LASH event of type %d.", lash_event_get_type(event));
				lash_event_destroy(event);
		}
	}

	return TRUE;
}

static void
init_lash(lash_args_t *args)
{
	/* XXX: Am I doing the right thing wrt protocol version? */
	lash_client = lash_init(args, PROGRAM_NAME, LASH_Config_Data_Set, LASH_PROTOCOL(2, 0));

	if (!lash_server_connected(lash_client)) {
		g_critical("Cannot initialize LASH.  Continuing anyway.");
		/* exit(EX_UNAVAILABLE); */

		return;
	}

	/* Schedule a function to process LASH events, ten times per second. */
	g_timeout_add(100, lash_callback, NULL);
}

#endif /* WITH_LASH */

/*
 * This is neccessary for exiting due to jackd being killed, when exit(0)
 * in process_callback won't get called for obvious reasons.
 */
gboolean
emergency_exit_timeout(gpointer notused)
{
	if (ctrl_c_pressed == 0)
		return TRUE;

	exit(0);
}

void
ctrl_c_handler(int signum)
{
	ctrl_c_pressed = 1;
}

static void
log_handler(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer notused)
{
	fprintf(stderr, "%s: %s\n", log_domain, message);
}

static void
show_version(void)
{
	fprintf(stdout, "%s %s, libsmf %s\n", PROGRAM_NAME, PROGRAM_VERSION, smf_get_version());

	exit(EX_OK);
}

static void
usage(void)
{
	fprintf(stderr, "usage: jack-smf-player [-dnqstlV] [ -a <input port>] [-r <rate>] file_name\n");

	exit(EX_USAGE);
}

int 
main(int argc, char *argv[])
{
	int ch;
	char *file_name, *autoconnect_port_name = NULL;

#ifdef WITH_LASH
	lash_args_t *lash_args;
#endif

#ifdef WITH_LASH
	lash_args = lash_extract_args(&argc, &argv);
#endif

	g_log_set_default_handler(log_handler, NULL);

	while ((ch = getopt(argc, argv, "a:dnqr:stlV")) != -1) {
		switch (ch) {
			case 'a':
				autoconnect_port_name = strdup(optarg);
				break;

			case 'd':
				drop_messages = 1;
				break;

			case 'n':
				start_stopped = 1;
				break;

			case 'q':
				be_quiet = 1;
				break;

			case 'r':
				rate_limit = strtod(optarg, NULL);
				if (rate_limit <= 0.0) {
					g_critical("Invalid rate limit specified.\n");

					exit(EX_USAGE);
				}

				break;

			case 's':
				just_one_output = 1;
				break;

			case 't':
				use_transport = 0;
				break;

			case 'l':
				loop_forever = 1;
				break;

			case 'V':
				show_version();
				break;

			case '?':
			default:
				usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argv[0] == NULL) {
		g_critical("No file name given.");
		usage();
	}

	file_name = argv[0];

	smf = smf_load(file_name);

	if (smf == NULL) {
		g_critical("Loading SMF file failed.");

		exit(-1);
	}

	if (!be_quiet)
		g_message("%s.", smf_decode(smf));

	if (smf->number_of_tracks > MAX_NUMBER_OF_TRACKS) {
		g_warning("Number of tracks (%d) exceeds maximum for per-track output; implying '-s' option.", smf->number_of_tracks);
		just_one_output = 1;
	}

#ifdef WITH_LASH
	init_lash(lash_args);
#endif

	g_timeout_add(1000, emergency_exit_timeout, (gpointer)0);
	signal(SIGINT, ctrl_c_handler);

	init_jack();

	if (autoconnect_port_name) {
		if (connect_to_input_port(autoconnect_port_name)) {
			g_critical("Couldn't connect to '%s', exiting.", autoconnect_port_name);
			exit(EX_UNAVAILABLE);
		}
	}

	if (use_transport && !start_stopped) {
		jack_transport_locate(jack_client, 0);
		jack_transport_start(jack_client);
	}

	if (!use_transport)
		playback_started = jack_frame_time(jack_client);

	//Flag client activation
	g_message("Ready to Play...");

	g_main_loop_run(g_main_loop_new(NULL, TRUE));

	/* Not reached. */

	return 0;
}
