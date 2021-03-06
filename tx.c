/*
 * Copyright (C) 2012 Mark Hills <mark@xwax.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#include <netdb.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <opus/opus.h>
#include <ortp/ortp.h>
#include <stdlib.h>
#include <signal.h> //Recupération et envoi de signaux entre processus


#include "defaults.h"
#include "device.h"
#include "notice.h"
#include "sched.h"
#include "multi.h"
#include "multistructure.h"

extern unsigned int verbose;
extern Client tx_client;

static RtpSession* create_rtp_send(const char *addr_desc, const int port)
{
	RtpSession *session;

	session = rtp_session_new(RTP_SESSION_SENDONLY);
	assert(session != NULL);

	rtp_session_set_scheduling_mode(session, 0);
	rtp_session_set_blocking_mode(session, 0);
	rtp_session_set_connected_mode(session, FALSE);
	if (rtp_session_set_remote_addr(session, addr_desc, port) != 0)
		abort();
	if (rtp_session_set_payload_type(session, 0) != 0)
		abort();
	if (rtp_session_set_multicast_ttl(session, 16) != 0)
		abort();

	return session;
}

static int send_one_frame(snd_pcm_t *snd,
		const unsigned int channels,
		const snd_pcm_uframes_t samples,
		OpusEncoder *encoder,
		const size_t bytes_per_frame,
		const unsigned int ts_per_frame,
		RtpSession *session)
{
	float *pcm;
	void *packet;
	ssize_t z;
	snd_pcm_sframes_t f;
	static unsigned int ts = 0;

	pcm = alloca(sizeof(float) * samples * channels);
	packet = alloca(bytes_per_frame);

	f = snd_pcm_readi(snd, pcm, samples);
	if (f < 0) {
		f = snd_pcm_recover(snd, f, 0);
		if (f < 0) {
			aerror("snd_pcm_readi", f);
			return -1;
		}
		return 0;
	}

	/* Opus encoder requires a complete frame, so if we xrun
	 * mid-frame then we discard the incomplete audio. The next
	 * read will catch the error condition and recover */

	if (f < samples) {
		fprintf(stderr, "Short read, %ld\n", f);
		return 0;
	}

	z = opus_encode_float(encoder, pcm, samples, packet, bytes_per_frame);
	if (z < 0) {
		fprintf(stderr, "opus_encode_float: %s\n", opus_strerror(z));
		return -1;
	}

	rtp_session_send_with_ts(session, packet, z, ts);
	ts += ts_per_frame;

	return 0;
}

static int run_tx(snd_pcm_t *snd,
		const unsigned int channels,
		const snd_pcm_uframes_t frame,
		OpusEncoder *encoder,
		const size_t bytes_per_frame,
		const unsigned int ts_per_frame,
		RtpSession *session)
{
	for (;;) {
		int r;

		r = send_one_frame(snd, channels, frame,
				encoder, bytes_per_frame, ts_per_frame,
				session);
		if (r == -1)
			return -1;

		if (verbose > 1)
			fputc('>', stderr);
	}
}

static void usage(FILE *fd)
{
	fprintf(fd, "Usage: tx [<parameters>]\n"
		"Real-time audio transmitter over IP\n");

	fprintf(fd, "\nAudio device (ALSA) parameters:\n");
	fprintf(fd, "  -d <dev>    Device name (default '%s')\n",
		DEFAULT_DEVICE);
	fprintf(fd, "  -m <ms>     Buffer time (default %d milliseconds)\n",
		DEFAULT_BUFFER);

	fprintf(fd, "\nNetwork parameters:\n");
	fprintf(fd, "  -h <addr>   IP address to send to (default %s)\n",
		DEFAULT_ADDR);
	fprintf(fd, "  -p <port>   UDP port number (default %d)\n",
		DEFAULT_PORT);

	fprintf(fd, "\nEncoding parameters:\n");
	fprintf(fd, "  -r <rate>   Sample rate (default %dHz)\n",
		DEFAULT_RATE);
	fprintf(fd, "  -c <n>      Number of channels (default %d)\n",
		DEFAULT_CHANNELS);
	fprintf(fd, "  -f <n>      Frame size (default %d samples, see below)\n",
		DEFAULT_FRAME);
	fprintf(fd, "  -b <kbps>   Bitrate (approx., default %d)\n",
		DEFAULT_BITRATE);

	fprintf(fd, "\nProgram parameters:\n");
	fprintf(fd, "  -v <n>      Verbosity level (default %d)\n",
		DEFAULT_VERBOSE);
	fprintf(fd, "  -D <file>   Run as a daemon, writing process ID to the given file\n");
	fprintf(fd, "  -w          If no slot available on the server, wait in wait list (default %s)\n", 
		DEFAULT_CLIENT_WAIT ? "ENABLED" : "DISABLED");

	fprintf(fd, "\nAllowed frame sizes (-f) are defined by the Opus codec. For example,\n"
		"at 48000Hz the permitted values are 120, 240, 480 or 960.\n");
}

int main(int argc, char *argv[])
{
	int r, error;
	size_t bytes_per_frame;
	unsigned int ts_per_frame;
	snd_pcm_t *snd;
	OpusEncoder *encoder;
	RtpSession *session;
	pid_t txpid;
	char log_message[200];

	/* command-line options */
	const char *device = DEFAULT_DEVICE,
		*addr = DEFAULT_ADDR,
		*pid = NULL;
	unsigned int buffer = DEFAULT_BUFFER,
		rate = DEFAULT_RATE,
		channels = DEFAULT_CHANNELS,
		frame = DEFAULT_FRAME,
		kbps = DEFAULT_BITRATE,
		port = DEFAULT_PORT,
		wait = DEFAULT_CLIENT_WAIT;

	fputs(COPYRIGHT "\n", stderr);

	for (;;) {
		int c;

		c = getopt(argc, argv, "b:c:d:f:h:m:p:r:v:D:w");
		if (c == -1)
			break;

		switch (c) {
		case 'b':
			kbps = atoi(optarg);
			break;
		case 'c':
			channels = atoi(optarg);
			break;
		case 'd':
			device = optarg;
			break;
		case 'f':
			frame = atol(optarg);
			break;
		case 'h':
			addr = optarg;
			break;
		case 'm':
			buffer = atoi(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'r':
			rate = atoi(optarg);
			break;
		case 'v':
			verbose = atoi(optarg);
			break;
		case 'D':
			pid = optarg;
			break;
		case 'w':
		        wait = 1;
			break;
		default:
			usage(stderr);
			return -1;
		}
	}
	
	snprintf(tx_client.name, 100, "Copain");
	tx_client.rate = kbps;//En attendant le fichier de config
	/* Follow the RFC, payload 0 has 8kHz reference rate */
	SOCKET mainSock = client_connection_init(addr);
	int p = slot_client_ask(mainSock);
	while(p >= 0) {
	    if(p > 0) { //La connexion retourne un port
		port = p;
		snprintf(log_message, 200, "Launching tx session on %s:%d", addr, p);
		log_add(log_message, stdout);
		txpid = fork();
		if(txpid == -1) {
		    /* Erreur */
		}
		else if(txpid == 0) { //On est dans le processus fils
		    encoder = opus_encoder_create(rate, channels, OPUS_APPLICATION_AUDIO,
				    &error);
		    if (encoder == NULL) {
			    fprintf(stderr, "opus_encoder_create: %s\n",
				    opus_strerror(error));
			    return -1;
		    }

		    bytes_per_frame = kbps * 1024 * frame / rate / 8;

		    ts_per_frame = frame * 8000 / rate;

		    ortp_init();
		    ortp_scheduler_init();
		    ortp_set_log_level_mask(ORTP_WARNING|ORTP_ERROR);
		    session = create_rtp_send(addr, port);
		    assert(session != NULL);

		    r = snd_pcm_open(&snd, device, SND_PCM_STREAM_CAPTURE, 0);
		    if (r < 0) {
			    aerror("snd_pcm_open", r);
			    return -1;
		    }
		    if (set_alsa_hw(snd, rate, channels, buffer * 1000) == -1)
			    return -1;
		    if (set_alsa_sw(snd) == -1)
			    return -1;

		    if (pid)
			    go_daemon(pid);
		    
		    log_add("Started audio transmission", stdout);
		    go_realtime();
		    r = run_tx(snd, channels, frame, encoder, bytes_per_frame,
			    ts_per_frame, session);

		    if (snd_pcm_close(snd) < 0)
			    abort();

		    rtp_session_destroy(session);
		    ortp_exit();
		    ortp_global_stats_display();

		    opus_encoder_destroy(encoder);
		}	    
	    }

	    else if(p == 0){ //Serveur plein mais liste d'attente
		if(!wait) {
		    log_add("Waiting mode disabled. Start tx with -w option to wait", stdout);		
		    socket_close(mainSock);
		    p = -1;
		    return 0;
		}
		sleep(1);
		socket_send(mainSock, "wait\0");
	    }
	    p = client_listen(mainSock);
	}
	
	socket_close(mainSock);
	if(kill(txpid, SIGTERM) == -1) {
	    perror("kill()");
	    exit(errno);
	}
	log_add("Audio transmission finished", stdout);
	
	

	return r;
}
