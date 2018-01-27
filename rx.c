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
#include <sys/socket.h>
#include <sys/types.h>
#include <stdlib.h>
#include <signal.h> //Recupération et envoi de signaux entre processus

#include "defaults.h"
#include "device.h"
#include "notice.h"
#include "sched.h"
#include "multi.h"

static unsigned int verbose = DEFAULT_VERBOSE;

static void timestamp_jump(RtpSession *session, ...)
{
	if (verbose > 1)
		fputc('|', stderr);
	rtp_session_resync(session);
}

static RtpSession* create_rtp_recv(const char *addr_desc, const int port,
		unsigned int jitter)
{
	RtpSession *session;

	session = rtp_session_new(RTP_SESSION_RECVONLY);
	rtp_session_set_scheduling_mode(session, FALSE);
	rtp_session_set_blocking_mode(session, FALSE);
	rtp_session_set_local_addr(session, addr_desc, port, -1);
	rtp_session_set_connected_mode(session, FALSE);
	rtp_session_enable_adaptive_jitter_compensation(session, TRUE);
	rtp_session_set_jitter_compensation(session, jitter); /* ms */
	rtp_session_set_time_jump_limit(session, jitter * 16); /* ms */
	if (rtp_session_set_payload_type(session, 0) != 0)
		abort();
	if (rtp_session_signal_connect(session, "timestamp_jump",
					timestamp_jump, 0) != 0)
	{
		abort();
	}

	return session;
}

static int play_one_frame(void *packet,
		size_t len,
		OpusDecoder *decoder,
		snd_pcm_t *snd,
		const unsigned int channels)
{
	int r;
	float *pcm;
	snd_pcm_sframes_t f, samples = 1920;

	pcm = alloca(sizeof(float) * samples * channels);

	if (packet == NULL) {
		r = opus_decode_float(decoder, NULL, 0, pcm, samples, 1);
	} else {
		r = opus_decode_float(decoder, packet, len, pcm, samples, 0);
	}
	if (r < 0) {
		fprintf(stderr, "opus_decode: %s\n", opus_strerror(r));
		return -1;
	}

	f = snd_pcm_writei(snd, pcm, r);
	if (f < 0) {
		f = snd_pcm_recover(snd, f, 0);
		if (f < 0) {
			aerror("snd_pcm_writei", f);
			return -1;
		}
		return 0;
	}
	if (f < r)
		fprintf(stderr, "Short write %ld\n", f);

	return r;
}

static int run_rx(RtpSession *session,
		OpusDecoder *decoder,
		snd_pcm_t *snd,
		const unsigned int channels,
		const unsigned int rate)
{
	int ts = 0;

	for (;;) {
		int r, have_more;
		char buf[32768];
		void *packet;

		r = rtp_session_recv_with_ts(session, (uint8_t*)buf,
				sizeof(buf), ts, &have_more);
		assert(r >= 0);
		assert(have_more == 0);
		if (r == 0) {
			packet = NULL;
			if (verbose > 1)
				fputc('#', stderr);
		} else {
			packet = buf;
			if (verbose > 1)
				fputc('.', stderr);
		}

		r = play_one_frame(packet, r, decoder, snd, channels);
		if (r == -1)
			return -1;

		/* Follow the RFC, payload 0 has 8kHz reference rate */

		ts += r * 8000 / rate;
	}
}

static void usage(FILE *fd)
{
	fprintf(fd, "Usage: rx [<parameters>]\n"
		"Real-time audio receiver over IP\n");

	fprintf(fd, "\nAudio device (ALSA) parameters:\n");
	fprintf(fd, "  -d <dev>    Device name (default '%s')\n",
		DEFAULT_DEVICE);
	fprintf(fd, "  -m <ms>     Buffer time (default %d milliseconds)\n",
		DEFAULT_BUFFER);

	fprintf(fd, "\nNetwork parameters:\n");
	fprintf(fd, "  -h <addr>   IP address to listen on (default %s)\n",
		DEFAULT_ADDR);
	fprintf(fd, "  -p <port>   UDP port number (default %d)\n",
		DEFAULT_PORT);
	fprintf(fd, "  -j <ms>     Jitter buffer (default %d milliseconds)\n",
		DEFAULT_JITTER);
        fprintf(fd, "  -i <n>      Amount of receiver instances lauch (default % instances)\n",
                DEFAULT_INSTANCES);

	fprintf(fd, "\nEncoding parameters (must match sender):\n");
	fprintf(fd, "  -r <rate>   Sample rate (default %dHz)\n",
		DEFAULT_RATE);
	fprintf(fd, "  -c <n>      Number of channels (default %d)\n",
		DEFAULT_CHANNELS);

	fprintf(fd, "\nProgram parameters:\n");
	fprintf(fd, "  -v <n>      Verbosity level (default %d)\n",
		DEFAULT_VERBOSE);
	fprintf(fd, "  -D <file>   Run as a daemon, writing process ID to the given file\n");
}

int main(int argc, char *argv[])
{
	int r, i, error;
	snd_pcm_t *snd;
	OpusDecoder *decoder;
	RtpSession *session;
	pid_t rxpid;
	
	/* command-line options */
	const char *device = DEFAULT_DEVICE,
		*addr = DEFAULT_ADDR,
		*pid = NULL;
	unsigned int buffer = DEFAULT_BUFFER,
		rate = DEFAULT_RATE,
		jitter = DEFAULT_JITTER,
		channels = DEFAULT_CHANNELS,
		port = DEFAULT_PORT,
                instances = DEFAULT_INSTANCES;

	fputs(COPYRIGHT "\n", stderr);

	for (;;) {
		int c;

		c = getopt(argc, argv, "c:d:h:j:m:p:r:v:i:");
		if (c == -1)
			break;
		switch (c) {
		case 'c':
			channels = atoi(optarg);
			break;
		case 'd':
			device = optarg;
			break;
		case 'h':
			addr = optarg;
			break;
		case 'j':
			jitter = atoi(optarg);
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
                case 'i': //nombre d'instances demandées par l'utilisateur au lancement du serveur
                        instances = atoi(optarg);
                        break;
		default:
			usage(stderr);
			return -1;
		}
	}

	decoder = opus_decoder_create(rate, channels, &error);
	if (decoder == NULL) {
		fprintf(stderr, "opus_decoder_create: %s\n",
			opus_strerror(error));
		return -1;
	}
        Slot *slots = malloc(instances * sizeof(Slot)); //initialisation du tableau de ports
        int currentPort = port; //premier port demandé par l'utilisateur, on l'incrementera à chaque nouveau slot créé
        
	if(slots != NULL) {
	    for(i=0;i<instances;i++) { //création de la plage de ports selon le nombre d'instances demandé
		slots[i].pid = 0;
		slots[i].portNumber = currentPort;
		slots[i].isFree = 1; //un port est libre par défaut
		currentPort++;
		fprintf(stdout, "Slot %d créée au port %d.\n",i+1, slots[i].portNumber);
	    }
        }
	else {
	    fprintf(stderr, "Memoire insuffisante");
	}
	ortp_init(); //Initialisation opus
	ortp_scheduler_init();
        
	for(i=0;i<instances;i++) {
	    /* Lancement d'un certain nombre de sessions à différents ports */
	    rxpid = fork(); //nouveau processus
	    
	    if(rxpid == -1) {
		/* Erreur */
	    }
	    else if(rxpid == 0) { //On est dans le processus fils
		    //Creation d'une session de lecture
		slots[i].pid = rxpid;
		session = create_rtp_recv(addr, slots[i].portNumber, jitter); //1 Session oRTP créée
		assert(session != NULL);

		r = snd_pcm_open(&snd, device, SND_PCM_STREAM_PLAYBACK, 0); //Ouverture d'une liaison carte son
		if (r < 0) {
			aerror("snd_pcm_open", r);
			return -1;
		}
		if (set_alsa_hw(snd, rate, channels, buffer * 1000) == -1)
			return -1;
		if (set_alsa_sw(snd) == -1)
			return -1;

		if (pid)
			go_daemon(pid); //Mise du recepteur en daemon si presence pid (Pere / fils ?)

		go_realtime();
		r = run_rx(session, decoder, snd, channels, rate); // boucle infinie de lecture



		if (snd_pcm_close(snd) < 0)
			abort(); //Si erreur de liaison carte son, fin de la boucle

		rtp_session_destroy(session); //Fin de la session oRTP

	    //Fin de la session oRTP
	    }
	    
	}
	for(;;) {
	    /* Boucle d'attente */
	}
	
	for(i=0;i<instances;i++) {
	    kill(slots[i].pid, SIGTERM);
	}
	ortp_exit();
	
	opus_decoder_destroy(decoder);
        
        free(slots);

	return r;
}