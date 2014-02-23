/*  XMMS2 - X Music Multiplexer System
 *  Copyright (C) 2003-2013 XMMS2 Team
 *
 *  PLUGINS ARE NOT CONSIDERED TO BE DERIVED WORK !!!
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  AdPlug input plugin for XMMS2
 */


#include "xmms/xmms_xformplugin.h"
#include "xmms/xmms_sample.h"
#include "xmms/xmms_medialib.h"
#include "xmms/xmms_log.h"
#include <adplug/adplug.h>
#include <adplug/emuopl.h>
#include <adplug/kemuopl.h>
#include <adplug/surroundopl.h>
#include <adplug/silentopl.h>

#include <glib.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>

/*
 * Standard AdPlug DB init code
 */

// Default file name of AdPlug's database file
#define ADPLUGDB_FILE		"adplug.db"

// Default AdPlug user's configuration subdirectory
#define ADPLUG_CONFDIR		".adplug"

// Default path to AdPlug's system-wide database file
#ifdef ADPLUG_DATA_DIR
#  define ADPLUGDB_PATH		ADPLUG_DATA_DIR "/" ADPLUGDB_FILE
#else
#  define ADPLUGDB_PATH		ADPLUGDB_FILE
#endif

static CAdPlugDatabase adplugDatabase;

/*
 * AdPlug file provider to "open" files from memory (put there by wherever
 * XMMS2 got the file from)
 */
#include <binio.h>
#include <binstr.h>
class CProvider_Mem: public CFileProvider
{
	private:
		uint8_t *file_data;
		int file_size;

	public:
		CProvider_Mem(uint8_t *file_data, int file_size) :
			file_data(file_data),
			file_size(file_size)
		{
		}

		virtual binistream *open(std::string filename) const;
		virtual void close(binistream *f) const;
};

binistream *CProvider_Mem::open(std::string filename) const
{
	binisstream *f = new binisstream(this->file_data, this->file_size);

	if (!f) return 0;
	if (f->error()) { delete f; return 0; }

	// Open all files as little endian with IEEE floats by default
	f->setFlag(binio::BigEndian, false); f->setFlag(binio::FloatIEEE);

	return f;
}

void CProvider_Mem::close(binistream *f) const
{
}

/*
 * Type definitions
 */
static const struct {
	const gchar *key;
	const gchar *value;
} config_params[] = {
	{"freq", "48000"},
	{"channels", "2"},
	{"enable_surround", "1"}
};

typedef struct xmms_adplug_data_St {
	struct {
		int freq, channels;
	} cfg;
	Copl *opl;
	CPlayer *player;
} xmms_adplug_data_t;

/*
 * Function prototypes
 */
static gboolean xmms_adplug_plugin_setup (xmms_xform_plugin_t *xform_plugin);
static gint xmms_adplug_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len, xmms_error_t *err);
static void xmms_adplug_destroy (xmms_xform_t *xform);
static gboolean xmms_adplug_init (xmms_xform_t *xform);
static gint64 xmms_adplug_seek (xmms_xform_t *xform, gint64 samples, xmms_xform_seek_mode_t whence, xmms_error_t *err);

/*
 * Plugin header
 */
XMMS_XFORM_PLUGIN_DEFINE ("adplug",
                          "AdPlug decoder", XMMS_VERSION,
                          "Adlib file synthesiser",
                          xmms_adplug_plugin_setup);

static gboolean
xmms_adplug_plugin_setup (xmms_xform_plugin_t *xform_plugin)
{
	xmms_xform_methods_t methods;

	XMMS_XFORM_METHODS_INIT (methods);
	methods.init = xmms_adplug_init;
	methods.destroy = xmms_adplug_destroy;
	methods.read = xmms_adplug_read;
	methods.seek = xmms_adplug_seek;

	xmms_xform_plugin_methods_set (xform_plugin, &methods);

	/*
	xmms_plugin_info_add (xform_plugin, "URL", "http://www.xmms.org/");
	xmms_plugin_info_add (xform_plugin, "Author", "Adam Nielsen <malvineous@shikadi.net>");
	xmms_plugin_info_add (xform_plugin, "License", "LGPL");
	*/

	/* Add all AdPlug's supported file extensions as custom MIME types.  For a
	 * CMF file, the MIME type "audio/x-adplug-cmf" will be added and associated
	 * with "*.cmf"
	 */
	GString *mime = g_string_new ("");
	GString *wildcard_ext = g_string_new ("");
	for (CPlayers::const_iterator i = CAdPlug::players.begin (); i != CAdPlug::players.end (); i++) {
		const char *ext;
		for (int j = 0; ext = (*i)->get_extension (j); j++) {

			g_string_assign (mime, "audio/x-adplug-");
			g_string_append (mime, &ext[1]); // trim off leading dot

			xmms_xform_plugin_indata_add (xform_plugin,
			                              XMMS_STREAM_TYPE_MIMETYPE,
			                              mime->str,
			/* Set the priority a little lower by default, as some
			 * formats (mid, s3m) can be handled by better plugins
			 */
			                              XMMS_STREAM_TYPE_PRIORITY,
			                              40,
			                              NULL);

			g_string_assign (wildcard_ext, "*");
			g_string_append (wildcard_ext, ext);
			xmms_magic_extension_add (mime->str, wildcard_ext->str);
		}
	}
	g_string_free (wildcard_ext, TRUE);
	g_string_free (mime, TRUE);

	for (int i = 0; i < G_N_ELEMENTS (config_params); i++) {
		xmms_xform_plugin_config_property_register (xform_plugin,
		                                            config_params[i].key,
		                                            config_params[i].value,
		                                            NULL, NULL);
	}

	/* Also add a special MIDI handler so we can play any MIDI format supported
	 * by XMMS2.  This priority is lower than the "real" MIDI synths so by
	 * default it will only be used if there is no other MIDI synth available.
	 */
	xmms_xform_plugin_indata_add (xform_plugin,
	                              XMMS_STREAM_TYPE_MIMETYPE,
	                              "audio/rawmidi",
	                              XMMS_STREAM_TYPE_PRIORITY,
	                              40,
	                              NULL);

	/* Add a special handler for IMF type-0 files, as these start with 0x00 bytes
	 * which are otherwise chopped off by the nulstripper plugin.
	 *
	 * It remains to be seen whether this incorrectly grabs other files with
	 * four leading nulls.
	 */
/*	xmms_magic_add ("id Software Music Format (type-0)", "audio/x-adplug-imf",
	                "0 long 0",
	                "4 byte >0",
	                NULL);
*/
	adplugDatabase.load(ADPLUGDB_PATH);
	CAdPlug::set_database(&adplugDatabase);

	return TRUE;
}

static void
xmms_adplug_destroy (xmms_xform_t *xform)
{
	xmms_adplug_data_t *data;
	xmms_config_property_t *cfgv;
	int i;

	g_return_if_fail (xform);

	data = (xmms_adplug_data_t *)xmms_xform_private_data_get (xform);
	g_return_if_fail (data);

	delete data->player;
	delete data->opl;

	g_free (data);

}

static gint64
xmms_adplug_seek (xmms_xform_t *xform, gint64 samples,
                  xmms_xform_seek_mode_t whence, xmms_error_t *err)
{
	xmms_adplug_data_t *data;

	g_return_val_if_fail (xform, FALSE);
	g_return_val_if_fail (whence == XMMS_XFORM_SEEK_SET, -1);

	data = (xmms_adplug_data_t *)xmms_xform_private_data_get (xform);
	g_return_val_if_fail (data, -1);

	gint64 ms = samples * 1000 / data->cfg.freq;
	data->player->seek (ms);

	/* There will be some rounding error, so return the value we ended up at
	 * rather than the value we were told to seek to.
	 */
	return ms * data->cfg.freq / 1000;
}

static gboolean
xmms_adplug_init (xmms_xform_t *xform)
{
	xmms_adplug_data_t *data;
	const gchar *metakey;
	gint filesize;
	xmms_config_property_t *cfgv;
	gint i;
	gboolean rawmidi = FALSE;
	gint enable_surround = 1;
	const gchar *url;
	GString *buffer;
	std::string title, artist, comment;

	g_return_val_if_fail (xform, FALSE);

	data = g_new0 (xmms_adplug_data_t, 1);

	cfgv = xmms_xform_config_lookup (xform, "freq");
	if (cfgv) data->cfg.freq = xmms_config_property_get_int (cfgv);

	cfgv = xmms_xform_config_lookup (xform, "channels");
	if (cfgv) data->cfg.channels = xmms_config_property_get_int (cfgv);

	cfgv = xmms_xform_config_lookup (xform, "enable_surround");
	if (cfgv) enable_surround = xmms_config_property_get_int (cfgv);

	if ((data->cfg.channels == 2) && enable_surround) {
		Copl *a = new CEmuopl (data->cfg.freq, true /* 16 bit */, false /* mono */);
		Copl *b = new CEmuopl (data->cfg.freq, true /* 16 bit */, false /* mono */);
		data->opl = new CSurroundopl (a, b, true /* 16 bit */);
		/* CSurroundopl now owns a and b and will free upon destruction */
	} else {
		data->opl = new CEmuopl (data->cfg.freq, true /* 16 bit */,
		                         data->cfg.channels == 2);
	}

	data->opl->init ();

	xmms_xform_private_data_set (xform, data);

	xmms_xform_outdata_type_add (xform,
	                             XMMS_STREAM_TYPE_MIMETYPE,
	                             "audio/pcm",
	                             XMMS_STREAM_TYPE_FMT_FORMAT,
	                             XMMS_SAMPLE_FORMAT_S16,
	                             XMMS_STREAM_TYPE_FMT_CHANNELS,
	                             data->cfg.channels,
	                             XMMS_STREAM_TYPE_FMT_SAMPLERATE,
	                             data->cfg.freq,
	                             XMMS_STREAM_TYPE_END);

	buffer = g_string_new ("");
	if (!buffer) {
		goto error;
	}

	for (;;) {
		xmms_error_t error;
		gchar buf[4096];
		gint ret;

		ret = xmms_xform_read (xform, buf, sizeof (buf), &error);
		if (ret == -1) {
			XMMS_DBG ("Error reading file");
			goto error;
		}
		if (ret == 0) {
			break;
		}
		g_string_append_len (buffer, buf, ret);
	}

	if (strcmp(xmms_xform_indata_get_str (xform, XMMS_STREAM_TYPE_MIMETYPE),
		"audio/rawmidi") == 0) rawmidi = true;

	if (rawmidi) {
		/* This is raw MIDI data, append a .mid file header so AdPlug can read it */
		guint32 mtrk_len = buffer->len;
		gint32 ticks_per_quarter_note;

		if (!xmms_xform_auxdata_get_int (xform, "tempo", &ticks_per_quarter_note)) {
			XMMS_DBG ("xform auxdata value 'tempo' not set (bug in previous xform plugin)");
			goto error;
		}

		g_string_prepend_len (buffer, "MThd\x00\x00\x00\x06\x00\x00\x00\x01__MTrk____", 22);

		/* Fill in the MIDI header values (big endian) */
		buffer->str[12] = (ticks_per_quarter_note >>  8) & 0xFF;
		buffer->str[13] = (ticks_per_quarter_note)       & 0xFF;

		buffer->str[18] = (mtrk_len >> 24) & 0xFF;
		buffer->str[19] = (mtrk_len >> 16) & 0xFF;
		buffer->str[20] = (mtrk_len >>  8) & 0xFF;
		buffer->str[21] = (mtrk_len)       & 0xFF;

		url = "test.mid"; // TODO
	} else {
		/* We need to get the filename so that AdPlug can use the extension to
		 * figure out what file type it is.  Luckily passing a URL as-is will still
		 * work.
		 */
		url = xmms_xform_get_url(xform);
	}
	XMMS_DBG("url is %s", url);

	{ /* Protect prMem and buffer */
		CProvider_Mem prMem ((uint8_t*)buffer->str, (int)buffer->len);
		data->player = CAdPlug::factory (url, data->opl, CAdPlug::players, prMem);
		if (!data->player) {
			XMMS_DBG ("AdPlug: invalid filetype");
			goto error;
		}

		/* Create a separate (silent) OPL synth and player instance, and use it to
		 * calculate the song length.  We can't use the existing OPL instance for
		 * this, otherwise there will be notes already playing (fading out) when
		 * we begin playback.
		 */
		Copl *dummy_opl;
		CPlayer *dummy_player;
		dummy_opl = new CSilentopl ();
		dummy_opl->init ();
		dummy_player = CAdPlug::factory (url, dummy_opl, CAdPlug::players, prMem);
		if (dummy_player) {
			metakey = XMMS_MEDIALIB_ENTRY_PROPERTY_DURATION;
			xmms_xform_metadata_set_int (xform, metakey, dummy_player->songlength (0));
		}
		delete dummy_player;
		delete dummy_opl;
	}
	/* Now prMem is freed and the AdPlug player instance has taken a copy of the
	 * song data, we can release the buffer.
	 */
	g_string_free (buffer, TRUE);
	buffer = NULL;

	if (!rawmidi) {
		// Only set these when we're reading the original file - the MIDI readers
		// will handle this when we get rawmidi data.

		metakey = XMMS_MEDIALIB_ENTRY_PROPERTY_TITLE;
		title = data->player->gettitle ();
		if (!title.empty()) {
			xmms_xform_metadata_set_str (xform, metakey, title.c_str());
		}

		metakey = XMMS_MEDIALIB_ENTRY_PROPERTY_ARTIST;
		artist = data->player->getauthor ();
		if (!artist.empty()) {
			xmms_xform_metadata_set_str (xform, metakey, artist.c_str());
		}

		metakey = XMMS_MEDIALIB_ENTRY_PROPERTY_COMMENT;
		comment = data->player->getdesc ();
		if (!comment.empty()) {
			xmms_xform_metadata_set_str (xform, metakey, comment.c_str());
		}
	}

	/* TODO: Can we set this somewhere in the medialib? (maybe in the MIME type section?) */
	XMMS_DBG ("File is of type %s", data->player->gettype ().c_str ());

	return TRUE;

error:

	delete data->opl;

	if (buffer)
		g_string_free (buffer, TRUE);

	g_free (data);

	return FALSE;
}


static gint
xmms_adplug_read (xmms_xform_t *xform, xmms_sample_t *buf, gint len, xmms_error_t *err)
{
	xmms_adplug_data_t *data;

	data = (xmms_adplug_data_t *)xmms_xform_private_data_get (xform);
	g_return_val_if_fail (data, 0);

	int sampsize = 2 /* 16 bit */ * ((data->cfg.channels == 1) ? 1 : 2);

	static long minicnt = 0;
	long i, towrite = len / sampsize;
	uint8_t *pos = (uint8_t *)buf;
	bool playing = true;

	/* Prepare audiobuf with emulator output */
	while (towrite > 0) {
		while (minicnt <= 0) {
			minicnt += data->cfg.freq;
			if (!data->player->update()) return 0; /* end of song */
		}
		i = MIN (towrite, (long)(minicnt / data->player->getrefresh () + 4) & ~3);
		data->opl->update ((short *)pos, i);
		pos += i * sampsize;
		towrite -= i;
		minicnt -= (long)(data->player->getrefresh () * i);
	}

	return len;
}
