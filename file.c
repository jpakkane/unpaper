// Copyright © 2005-2007 Jens Gulden
// Copyright © 2011-2011 Diego Elio Pettenò
// SPDX-FileCopyrightText: 2005 The unpaper authors
//
// SPDX-License-Identifier: GPL-2.0-only

/* --- tool functions for file handling ------------------------------------ */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "tools.h"
#include "unpaper.h"

/**
 * Loads image data from a file in pnm format.
 *
 * @param f file to load
 * @param image structure to hold loaded image
 * @param type returns the type of the loaded image
 */
void loadImage(const char *filename, AVFrame **image) {
  int ret, got_frame = 0;
  AVFormatContext *s = NULL;
  AVCodecContext *avctx = avcodec_alloc_context3(NULL);
  AVCodec *codec;
  AVPacket pkt;
  AVFrame *frame = av_frame_alloc();
  char errbuff[1024];

  if (!avctx)
    errOutput("cannot allocate a new context");

  ret = avformat_open_input(&s, filename, NULL, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuff, sizeof(errbuff));
    errOutput("unable to open file %s: %s", filename, errbuff);
  }

  avformat_find_stream_info(s, NULL);

  if (verbose >= VERBOSE_MORE)
    av_dump_format(s, 0, filename, 0);

  if (s->nb_streams < 1)
    errOutput("unable to open file %s: missing streams", filename);

  if (s->streams[0]->codec->codec_type != AVMEDIA_TYPE_VIDEO)
    errOutput("unable to open file %s: wrong stream", filename);

  ret = avcodec_copy_context(avctx, s->streams[0]->codec);
  if (ret < 0) {
    av_strerror(ret, errbuff, sizeof(errbuff));
    errOutput("cannot set the new context for %s: %s", filename, errbuff);
  }

  codec = avcodec_find_decoder(avctx->codec_id);
  if (!codec)
    errOutput("unable to open file %s: unsupported format", filename);

  ret = avcodec_open2(avctx, codec, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuff, sizeof(errbuff));
    errOutput("unable to open file %s: %s", filename, errbuff);
  }

  ret = av_read_frame(s, &pkt);
  if (ret < 0) {
    av_strerror(ret, errbuff, sizeof(errbuff));
    errOutput("unable to open file %s: %s", filename, errbuff);
  }

  if (pkt.stream_index != 0)
    errOutput("unable to open file %s: invalid stream.", filename);

  ret = avcodec_decode_video2(avctx, frame, &got_frame, &pkt);
  if (ret < 0) {
    av_strerror(ret, errbuff, sizeof(errbuff));
    errOutput("unable to open file %s: %s", filename, errbuff);
  }

  switch (frame->format) {
  case AV_PIX_FMT_Y400A: // 8-bit grayscale PNG
  case AV_PIX_FMT_GRAY8:
  case AV_PIX_FMT_RGB24:
  case AV_PIX_FMT_MONOBLACK:
  case AV_PIX_FMT_MONOWHITE:
    *image = av_frame_clone(frame);
    break;

  case AV_PIX_FMT_PAL8:
    initImage(image, frame->width, frame->height, AV_PIX_FMT_RGB24, -1);

    const uint32_t *palette = (const uint32_t *)frame->data[1];
    for (int y = 0; y < frame->height; y++) {
      for (int x = 0; x < frame->width; x++) {
        const uint8_t palette_index =
            frame->data[0][frame->linesize[0] * y + x];
        setPixel(palette[palette_index], x, y, *image);
      }
    }
    break;

  default:
    errOutput("unable to open file %s: unsupported pixel format", filename);
  }

  avcodec_close(avctx);
  av_free(avctx);
  avformat_close_input(&s);
}

/**
 * Saves image data to a file in pgm or pbm format.
 *
 * @param filename file name to save image to
 * @param image image to save
 * @param type filetype of the image to save
 * @return true on success, false on failure
 */
void saveImage(char *filename, AVFrame *input, int outputPixFmt) {
  AVOutputFormat *fmt = NULL;
  enum AVCodecID output_codec = -1;
  AVCodec *codec;
  AVFormatContext *out_ctx;
  AVCodecContext *codec_ctx;
  AVStream *video_st;
  AVFrame *output = input;
  int ret;
  char errbuff[1024];

  fmt = av_guess_format("image2", NULL, NULL);

  if (!fmt) {
    errOutput("could not find suitable output format.");
  }

  out_ctx = avformat_alloc_context();
  if (!out_ctx) {
    errOutput("unable to allocate output context.");
  }

  out_ctx->oformat = fmt;
  snprintf(out_ctx->filename, sizeof(out_ctx->filename), "%s", filename);

  switch (outputPixFmt) {
  case AV_PIX_FMT_RGB24:
    output_codec = AV_CODEC_ID_PPM;
    break;
  case AV_PIX_FMT_Y400A:
  case AV_PIX_FMT_GRAY8:
    outputPixFmt = AV_PIX_FMT_GRAY8;
    output_codec = AV_CODEC_ID_PGM;
    break;
  case AV_PIX_FMT_MONOBLACK:
  case AV_PIX_FMT_MONOWHITE:
    outputPixFmt = AV_PIX_FMT_MONOWHITE;
    output_codec = AV_CODEC_ID_PBM;
    break;
  }

  if (input->format != outputPixFmt) {
    initImage(&output, input->width, input->height, outputPixFmt, -1);
    copyImageArea(0, 0, input->width, input->height, input, 0, 0, output);
  }

  codec = avcodec_find_encoder(output_codec);
  if (!codec) {
    errOutput("output codec not found");
  }

  video_st = avformat_new_stream(out_ctx, codec);
  if (!video_st) {
    errOutput("could not alloc output stream");
  }

  codec_ctx = video_st->codec;
  codec_ctx->width = output->width;
  codec_ctx->height = output->height;
  codec_ctx->pix_fmt = output->format;
  video_st->time_base.den = codec_ctx->time_base.den = 1;
  video_st->time_base.num = codec_ctx->time_base.num = 1;

  ret = avcodec_open2(codec_ctx, codec, NULL);

  if (ret < 0) {
    av_strerror(ret, errbuff, sizeof(errbuff));
    errOutput("unable to open codec: %s", errbuff);
  }

  if (verbose >= VERBOSE_MORE)
    av_dump_format(out_ctx, 0, filename, 1);

  if (avio_open(&out_ctx->pb, filename, AVIO_FLAG_WRITE) < 0) {
    errOutput("could not open '%s'", filename);
  }

  if (avformat_write_header(out_ctx, NULL) < 0) {
    errOutput("error writing header to '%s'", filename);
  }

  AVPacket pkt = {0};
  int got_packet;
  av_init_packet(&pkt);

  /* encode the image */
  ret = avcodec_encode_video2(video_st->codec, &pkt, output, &got_packet);

  if (ret < 0) {
    av_strerror(ret, errbuff, sizeof(errbuff));
    errOutput("unable to write file %s: %s", filename, errbuff);
  }
  av_write_frame(out_ctx, &pkt);

  av_write_trailer(out_ctx);
  avcodec_close(codec_ctx);

  av_free(codec_ctx);
  av_free(video_st);

  avio_close(out_ctx->pb);
  av_free(out_ctx);

  if (output != input)
    av_frame_free(&output);
}

/**
 * Saves the image if full debugging mode is enabled.
 */
void saveDebug(char *filenameTemplate, int index, AVFrame *image) {
  if (verbose >= VERBOSE_DEBUG_SAVE) {
    char debugFilename[100];
    sprintf(debugFilename, filenameTemplate, index);
    saveImage(debugFilename, image, image->format);
  }
}
