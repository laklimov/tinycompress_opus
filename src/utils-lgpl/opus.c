//SPDX-License-Identifier: LGPL-2.1-only

//Copyright (c) 2025, Linaro Ltd

#include <stdint.h>
#include <linux/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <getopt.h>
#define __force
#define __user
#include "sound/compress_params.h"
#include "tinycompress/tinycompress.h"
#include <ctype.h>
#include <inttypes.h>
#include <oggz/oggz.h>	/* Needs liboggz version 1.1.3 or something like that */
			/* liboggz 1.1.1 is buggy, can't parse Opus packets.  */

#define OGGZ_BLOCK_SIZE		(1024 * 1024)
#define OPUS_PACKET_SIZE	4

struct opus_privata_data {
	struct compress *compress;
	bool start_done;
	bool verbose;
	int start_bytes;
	uint64_t total_bytes_written;
};

static int
read_opus_packet(OGGZ *oggz, oggz_packet *zp, long serialno, void *user_data)
{
	struct opus_privata_data *priv_data =
					(struct opus_privata_data *) user_data;
	struct compress *compress = priv_data->compress;
	ogg_packet *op = &zp->op;
	uint32_t packet_size = op->bytes;
	uint32_t total_len = packet_size + OPUS_PACKET_SIZE;
	bool verbose = priv_data->verbose;
	void *buff;
	int wrote;

	/* Feeding DSP Opus packets is a time-sensitive thingy,
	 * hence trying to do here as less as possible.
	 * If we're not fast enough the compress playback may stuck. */
	buff = malloc(total_len);
	if (buff == NULL)
		return -1;
	/* memset(buff, 0, total_len); */

	/* For Qualcomm audio DSP the Opus packets needs to be prepend with
	 * 4 bytes of length. Construct this into the buffer... */
	memcpy(buff, &packet_size, OPUS_PACKET_SIZE);
	memcpy(buff + sizeof(uint32_t), op->packet, op->bytes);

	/* ... and feed this buffer to DSP */
	wrote = compress_write(compress, buff, total_len);
	if (wrote < 0) {
		fprintf(stderr, "Error %d playing sample\n", wrote);
		fprintf(stderr, "ERR: %s\n", compress_get_error(compress));
		goto exit_free_buff;
	} else if (wrote != total_len) {
		/* TODO/possible bug: buffer pointer needs to be reset here */
		fprintf(stderr, "ERR: we tried to write %d, DSP accepted %d\n",
			total_len, wrote);
	}

	priv_data->total_bytes_written += wrote;

	/* If we wrote enough packets to compress device (to audio DSP), then
	 * we can actually start the compress playback i.e. ask DSP to
	 * start processing the Opus packets. */
	if (!priv_data->start_done &&
		priv_data->total_bytes_written >= priv_data->start_bytes) {

		if (verbose) {
			fprintf(stderr, "compress_start!"
				" Audio should start now.. \n");
			fprintf(stderr, "wrote bytes=%d,needed to write %d.\n",
				priv_data->total_bytes_written,
				priv_data->start_bytes);
		}
		/* kick the DSP to start the playback itself */
		compress_start(compress);
		priv_data->start_done = true;
	}

exit_free_buff:
	free(buff);

	return 0;
}

void play_opus(struct compress *compress, char *name, int size_to_start,
								int verbose)
{
	struct opus_privata_data priv_data;
	OGGZ * oggz;
	int retval;

	oggz = oggz_open(name, OGGZ_READ | OGGZ_AUTO);
	if (oggz == NULL) {
		fprintf(stderr, "%s: file or stream not found!\n", __func__);
		return;
	}

	/* prepare data that will be passed to read_opus_packet() callback */
	priv_data.compress = compress;
	priv_data.start_done = false;
	priv_data.start_bytes = size_to_start;
	priv_data.verbose = (bool) verbose;
	priv_data.total_bytes_written = 0;

	/* set the read_opus_packet() to be executed for each Opus packet */
	oggz_set_read_callback(oggz, -1, read_opus_packet, &priv_data);
	oggz_run_set_blocksize(oggz, OGGZ_BLOCK_SIZE);

	if (verbose)
		fprintf(stderr, "%s: run Ogg file parsing to get "
				"raw Opus packets.. \n", __func__);

	/* start the sequence of deconstructing Ogg file into raw Opus packets
	 * and executing callback read_opus_packet() for each Opus packet */
	retval = oggz_run(oggz);
	if (retval)
		fprintf(stderr, "%s: oggz_run() failed, returned = %i\n",
				__func__, retval);

	oggz_close(oggz);

	if (verbose)
		fprintf(stderr,
			"%s: finished playing opus packets.\n", __func__);
	return;
}

