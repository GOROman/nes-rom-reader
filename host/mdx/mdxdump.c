/*
 * mdxdump - MDX を portable_mdx (MXDRV) で解釈し、OPM レジスタ書き込みを
 *           タイムスタンプ付きイベント列としてテキスト出力する。
 *           音源エミュレーションの PCM 出力は捨てる(再生ロジックだけ使う)。
 *
 * 出力形式: 1行1イベント "t_us addr data"(10進, addr/dataは0-255)
 *
 * usage: mdxdump -i <file.mdx> [-t <max_seconds>] > out.evt
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>

#include <mdx_util.h>
#include <mxdrv.h>
#include <mxdrv_context.h>

/* sound_iocs.cpp に追加したフック */
void (*g_opmWriteHook)(int addr, int data) = NULL;

#define NUM_SAMPLES_PER_SEC 48000
#define CHUNK_SAMPLES 16   /* 333µs 粒度でタイムスタンプを刻む */

static uint64_t g_sampleCounter = 0;

static void opmHook(int addr, int data) {
	uint64_t t_us = g_sampleCounter * 1000000ULL / NUM_SAMPLES_PER_SEC;
	printf("%llu %d %d\n", (unsigned long long)t_us, addr, data);
}

static void *mallocReadFile(const char *fileName, uint32_t *sizeRet) {
	FILE *fd = fopen(fileName, "rb");
	if (fd == NULL) return NULL;
	struct stat stbuf;
	if (fstat(fileno(fd), &stbuf) == -1) { fclose(fd); return NULL; }
	uint32_t size = (uint32_t)stbuf.st_size;
	void *buffer = malloc(size);
	if (buffer == NULL) { fclose(fd); return NULL; }
	fread(buffer, 1, size, fd);
	*sizeRet = size;
	fclose(fd);
	return buffer;
}

int main(int argc, char **argv) {
	const char *mdxFilePath = NULL;
	int maxSeconds = 300;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) mdxFilePath = argv[++i];
		else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) maxSeconds = atoi(argv[++i]);
	}
	if (mdxFilePath == NULL) {
		fprintf(stderr, "usage: %s -i <file.mdx> [-t seconds] > out.evt\n", argv[0]);
		return 1;
	}

	uint32_t mdxFileImageSizeInBytes = 0;
	void *mdxFileImage = mallocReadFile(mdxFilePath, &mdxFileImageSizeInBytes);
	if (mdxFileImage == NULL) { fprintf(stderr, "read failed: %s\n", mdxFilePath); return 1; }

	char title[256];
	if (MdxGetTitle(mdxFileImage, mdxFileImageSizeInBytes, title, sizeof(title)))
		fprintf(stderr, "title: %s\n", title);

	/* PDX (ADPCM) は使わない(YM2151 実チップのみ) */
	uint32_t mdxBufferSizeInBytes = 0, pdxBufferSizeInBytes = 0;
	if (!MdxGetRequiredBufferSize(mdxFileImage, mdxFileImageSizeInBytes, 0,
	                              &mdxBufferSizeInBytes, &pdxBufferSizeInBytes)) {
		fprintf(stderr, "MdxGetRequiredBufferSize failed\n");
		return 1;
	}
	void *mdxBuffer = malloc(mdxBufferSizeInBytes);
	if (!MdxUtilCreateMdxPdxBuffer(mdxFileImage, mdxFileImageSizeInBytes,
	                               NULL, 0, mdxBuffer, mdxBufferSizeInBytes, NULL, 0)) {
		fprintf(stderr, "MdxUtilCreateMdxPdxBuffer failed\n");
		return 1;
	}
	free(mdxFileImage);

	#define MDX_BUFFER_SIZE   (1 * 1024 * 1024)
	#define PDX_BUFFER_SIZE   (2 * 1024 * 1024)
	#define MEMORY_POOL_SIZE  (8 * 1024 * 1024)
	MxdrvContext context;
	if (!MxdrvContext_Initialize(&context, MEMORY_POOL_SIZE)) {
		fprintf(stderr, "MxdrvContext_Initialize failed\n");
		return 1;
	}
	if (MXDRV_Start(&context, NUM_SAMPLES_PER_SEC, 0, 0, 0,
	                MDX_BUFFER_SIZE, PDX_BUFFER_SIZE, 0) != 0) {
		fprintf(stderr, "MXDRV_Start failed\n");
		return 1;
	}
	MXDRV_TotalVolume(&context, 256);
	if (MXDRV_SetData2(&context, mdxBuffer, mdxBufferSizeInBytes, NULL, 0) != 0) {
		fprintf(stderr, "MXDRV_SetData2 failed\n");
		return 1;
	}

	/* 曲長(1ループ+α)を測る。0 なら maxSeconds で打ち切り */
	uint32_t playTimeMs = MXDRV_MeasurePlayTime2(&context, 1, 1);
	fprintf(stderr, "play time: %u ms\n", playTimeMs);
	uint64_t maxSamples = (uint64_t)maxSeconds * NUM_SAMPLES_PER_SEC;
	if (playTimeMs > 0) {
		uint64_t s = (uint64_t)playTimeMs * NUM_SAMPLES_PER_SEC / 1000;
		if (s < maxSamples) maxSamples = s;
	}

	g_opmWriteHook = opmHook;
	MXDRV_Play2(&context);

	static int16_t pcmBuf[CHUNK_SAMPLES * 2];
	while (g_sampleCounter < maxSamples) {
		MXDRV_GetPCM(&context, pcmBuf, CHUNK_SAMPLES);
		g_sampleCounter += CHUNK_SAMPLES;
		if (MXDRV_GetTerminated(&context)) break;
	}
	fprintf(stderr, "dumped %llu samples (%.1f s)\n",
	        (unsigned long long)g_sampleCounter,
	        (double)g_sampleCounter / NUM_SAMPLES_PER_SEC);
	return 0;
}
