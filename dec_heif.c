/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / HEIF/AVIF image decoder filter
 *  based on libheif (https://github.com/strukturag/libheif)
 *
 */

#include <gpac/filters.h>
#include <string.h>
#include <libheif/heif.h>

typedef struct
{
	GF_FilterPid *ipid, *opid;

	Bool is_playing;
	u32 src_timescale;
	u32 codec_id;
	u32 ofmt;
} GF_HEIFDecCtx;

static GF_Err heifdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	GF_HEIFDecCtx *ctx = (GF_HEIFDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_CODECID);
	if (!prop)
		return GF_NOT_SUPPORTED;
	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}

	// copy properties at init or reconfig
	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));

	if (!ctx->ofmt)
	{
		ctx->ofmt = GF_PIXEL_RGB;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_RGB));
	}

	return GF_OK;
}

static GF_Err heifdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size;
	struct heif_error err;
	struct heif_context *heif;
	struct heif_image_handle *handle;
	struct heif_image *img;
	int width, height;
	enum heif_chroma chroma;
	GF_HEIFDecCtx *ctx = (GF_HEIFDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);

	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	heif = heif_context_alloc();
	if (!heif)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	err = heif_context_read_from_memory(heif, data, size, NULL);
	if (err.code != heif_error_Ok)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] heif_context_read_from_memory failed: %s\n", err.message));
		heif_context_free(heif);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	handle = NULL;
	err = heif_context_get_primary_image_handle(heif, &handle);
	if (err.code != heif_error_Ok || !handle)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] heif_context_get_primary_image_handle failed: %s\n", err.message));
		heif_context_free(heif);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	ctx->ofmt = heif_image_handle_has_alpha_channel(handle) ? GF_PIXEL_RGBA : GF_PIXEL_RGB;
	chroma = (ctx->ofmt == GF_PIXEL_RGBA) ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB;

	img = NULL;
	err = heif_decode_image(handle, &img, heif_colorspace_RGB, chroma, NULL);
	if (err.code != heif_error_Ok || !img)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] heif_decode_image failed: %s\n", err.message));
		heif_image_handle_release(handle);
		heif_context_free(heif);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	width = heif_image_get_width(img, heif_channel_interleaved);
	height = heif_image_get_height(img, heif_channel_interleaved);

	{
		size_t src_stride = 0;
		const u8 *src = heif_image_get_plane_readonly2(img, heif_channel_interleaved, &src_stride);
		u32 bytes_per_pixel = (ctx->ofmt == GF_PIXEL_RGBA) ? 4 : 3;
		u32 dst_stride = (u32)width * bytes_per_pixel;
		u32 out_size = dst_stride * (u32)height;

		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] DEBUG width=%d height=%d src=%p src_stride=%zu bytes_per_pixel=%u dst_stride=%u out_size=%u\n", width, height, src, src_stride, bytes_per_pixel, dst_stride, out_size));

		if (!src || width <= 0 || height <= 0)
		{
			heif_image_release(img);
			heif_image_handle_release(handle);
			heif_context_free(heif);
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_PIXFMT, &PROP_UINT(ctx->ofmt));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_WIDTH, &PROP_UINT((u32)width));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_HEIGHT, &PROP_UINT((u32)height));

		dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
		if (!dst_pck)
		{
			heif_image_release(img);
			heif_image_handle_release(handle);
			heif_context_free(heif);
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_OUT_OF_MEM;
		}

		{
			int row;
			for (row = 0; row < height; row++)
			{
				memcpy(output + row * dst_stride, src + row * src_stride, dst_stride);
			}
		}
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] DEBUG memcpy loop done\n"));
	}

	gf_filter_pck_merge_properties(pck, dst_pck);
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] DEBUG merge_properties done\n"));
	gf_filter_pck_set_dependency_flags(dst_pck, 0);
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] DEBUG set_dependency_flags done\n"));
	gf_filter_pck_send(dst_pck);
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] DEBUG pck_send done\n"));

	heif_image_release(img);
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] DEBUG image_release done\n"));
	heif_image_handle_release(handle);
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] DEBUG handle_release done\n"));
	heif_context_free(heif);
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] DEBUG context_free done\n"));
	gf_filter_pid_drop_packet(ctx->ipid);
	GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[HEIF] DEBUG drop_packet done\n"));

	return GF_OK;
}

static const GF_FilterCapability HEIFDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_4CC('H', 'E', 'I', 'F')),
		CAP_BOOL(GF_CAPS_INPUT_EXCLUDED, GF_PROP_PID_UNFRAMED, GF_TRUE),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister HEIFDecoderRegister = {
	.name = "heifdec",
	GF_FS_SET_DESCRIPTION("HEIF/AVIF image decoder")
		GF_FS_SET_HELP("This filter decodes HEIF/HEIC/AVIF images using libheif.")
			.private_size = sizeof(GF_HEIFDecCtx),
	SETCAPS(HEIFDecCaps),
	.configure_pid = heifdec_configure_pid,
	.process = heifdec_process,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE dynCall_heifdec_register(GF_FilterSession *session)
{
	return &HEIFDecoderRegister;
}


#include "filter_register.h"
__attribute__((constructor))
void register_heifdec(void) {
    gf_filter_auto_register("heifdec", dynCall_heifdec_register);
}
