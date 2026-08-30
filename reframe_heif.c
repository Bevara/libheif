/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / HEIF image reframer filter
 *  based on libheif (https://github.com/strukturag/libheif)
 *
 */

#include <gpac/filters.h>
#include <string.h>
#include <libheif/heif.h>

typedef struct
{
	// only one input pid declared
	GF_FilterPid *ipid;
	// only one output pid declared
	GF_FilterPid *opid;
	u32 src_timescale;
	Bool owns_timescale;
	u32 codec_id;

	Bool initial_play_done;
	Bool is_playing;
} GF_ReframeHeifCtx;

static GF_Err rfheif_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_ReframeHeifCtx *ctx = gf_filter_get_udta(filter);
	const GF_PropertyValue *p;

	if (is_remove)
	{
		ctx->ipid = NULL;
		return GF_OK;
	}

	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	gf_filter_pid_set_framing_mode(pid, GF_TRUE);
	ctx->ipid = pid;
	// force retest of codecid
	ctx->codec_id = 0;

	p = gf_filter_pid_get_property(pid, GF_PROP_PID_TIMESCALE);
	if (p)
		ctx->src_timescale = p->value.uint;

	if (ctx->src_timescale && !ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
		gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);
	}
	ctx->is_playing = GF_TRUE;
	return GF_OK;
}

static Bool rfheif_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_FilterEvent fevt;
	GF_ReframeHeifCtx *ctx = gf_filter_get_udta(filter);
	if (evt->base.on_pid != ctx->opid)
		return GF_TRUE;
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		if (ctx->is_playing)
		{
			return GF_TRUE;
		}

		ctx->is_playing = GF_TRUE;
		if (!ctx->initial_play_done)
		{
			ctx->initial_play_done = GF_TRUE;
			return GF_TRUE;
		}

		GF_FEVT_INIT(fevt, GF_FEVT_SOURCE_SEEK, ctx->ipid);
		fevt.seek.start_offset = 0;
		gf_filter_pid_send_event(ctx->ipid, &fevt);
		return GF_TRUE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		break;
	}
	// cancel all events
	return GF_TRUE;
}

static GF_Err rfheif_process(GF_Filter *filter)
{
	GF_ReframeHeifCtx *ctx = gf_filter_get_udta(filter);
	GF_FilterPacket *pck, *dst_pck;
	GF_Err e;
	u8 *data;
	u32 size;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			if (ctx->opid)
				gf_filter_pid_set_eos(ctx->opid);
			ctx->is_playing = GF_FALSE;
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);

	if (!ctx->opid || !ctx->codec_id)
	{
		u32 w = 0, h = 0;
		struct heif_error err;
		struct heif_context *heif = heif_context_alloc();
		if (!heif)
			return GF_OUT_OF_MEM;

		err = heif_context_read_from_memory(heif, data, size, NULL);
		if (err.code != heif_error_Ok)
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] heif_context_read_from_memory failed: %s\n", err.message));
			heif_context_free(heif);
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		struct heif_image_handle *handle = NULL;
		err = heif_context_get_primary_image_handle(heif, &handle);
		if (err.code != heif_error_Ok || !handle)
		{
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] heif_context_get_primary_image_handle failed: %s\n", err.message));
			heif_context_free(heif);
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		w = heif_image_handle_get_width(handle);
		h = heif_image_handle_get_height(handle);

		ctx->codec_id = GF_4CC('H', 'E', 'I', 'F');
		ctx->opid = gf_filter_pid_new(filter);
		if (!ctx->opid)
		{
			heif_image_handle_release(handle);
			heif_context_free(heif);
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_SERVICE_ERROR;
		}

		// we don't have input reconfig for now
		gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_VISUAL));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(ctx->codec_id));
		if (w)
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT(w));
		if (h)
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT(h));

		if (!gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_TIMESCALE))
		{
			gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(1000));
			ctx->owns_timescale = GF_TRUE;
		}

		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NB_FRAMES, &PROP_UINT(1));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PLAYBACK_MODE, &PROP_UINT(GF_PLAYBACK_MODE_FASTFORWARD));

		heif_image_handle_release(handle);
		heif_context_free(heif);
	}

	e = GF_OK;
	u32 start_offset = 0;

	dst_pck = gf_filter_pck_new_ref(ctx->opid, start_offset, size - start_offset, pck);
	if (!dst_pck)
		return GF_OUT_OF_MEM;

	gf_filter_pck_merge_properties(pck, dst_pck);
	if (ctx->owns_timescale)
	{
		gf_filter_pck_set_cts(dst_pck, 0);
		gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
		gf_filter_pck_set_duration(dst_pck, 1000);
	}

	gf_filter_pck_send(dst_pck);
	gf_filter_pid_drop_packet(ctx->ipid);

	return e;
}

static const char *rfheif_probe_data(const u8 *data, u32 size, GF_FilterProbeScore *score)
{
	enum heif_filetype_result res;

	if (size < 12)
		return NULL;

	res = heif_check_filetype(data, size);
	if (res == heif_filetype_yes_supported)
	{
		*score = GF_FPROBE_SUPPORTED;
		return "image/heif";
	}
	if (res == heif_filetype_maybe)
	{
		*score = GF_FPROBE_MAYBE_SUPPORTED;
		return "image/heif";
	}
	return NULL;
}

static const GF_FilterCapability ReframeHeifCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "heic|heics|heif|heifs|hif"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "image/heic|image/heif|image/heic-sequence|image/heif-sequence"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_4CC('H', 'E', 'I', 'F')),
};

GF_FilterRegister ReframeHeifRegister = {
	.name = "rfheif",
	GF_FS_SET_DESCRIPTION("HEIF image reframer")
		GF_FS_SET_HELP("This filter parses HEIF/HEIC (HEVC-coded) image files/data (via libheif) and outputs corresponding visual PID and frames. For AVIF (AV1-coded), use the avif;libaom filter pair instead.\n")
			.private_size = sizeof(GF_ReframeHeifCtx),
	SETCAPS(ReframeHeifCaps),
	.configure_pid = rfheif_configure_pid,
	.probe_data = rfheif_probe_data,
	.process = rfheif_process,
	.process_event = rfheif_process_event};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE dynCall_heif_reframe_register(GF_FilterSession *session)
{
	return &ReframeHeifRegister;
}


#include "filter_register.h"
__attribute__((constructor))
void register_heif_reframe(void) {
    gf_filter_auto_register("heif_reframe", dynCall_heif_reframe_register);
}
