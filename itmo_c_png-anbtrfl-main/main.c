#include "return_codes.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ZLIB)

#include <zlib.h>

#elif defined(LIBDEFLATE)

#include <libdeflate.h>

#elif defined(ISAL)

#include <include/igzip_lib.h>

#else
#error "expected library definition MACRO"
#endif

struct PNG_Chunk
{
	unsigned int length;
	char type[4];
	unsigned int crc;
	unsigned char data[];
};

struct PNG_Header
{
	unsigned int width;
	unsigned int height;
	unsigned char depth;
	unsigned char colType;
	unsigned short mustbe0;
	unsigned char interlaced;
};

#if defined(ZLIB)

bool decompress(unsigned char *src, unsigned char *dst, size_t srcSize, size_t decSize)
{
	int error = uncompress(dst, (uLongf *)&decSize, src, srcSize);
	return (error == Z_OK);
}

#elif defined(LIBDEFLATE)

bool decompress(unsigned char *src, unsigned char *dst, size_t srcSize, size_t decSize)
{
	struct libdeflate_decompressor *decompressor = libdeflate_alloc_decompressor();
	if (decompressor == NULL)
	{
		return false;
	}
	size_t s;
	int error = libdeflate_zlib_decompress(decompressor, src, srcSize, dst, decSize, &s);
	free(decompressor);
	return (error == LIBDEFLATE_SUCCESS);
}
#elif defined(ISAL)

bool decompress(unsigned char *src, unsigned char *dst, size_t srcSize, size_t decSize)
{
	struct inflate_state state;
	isal_inflate_init(&state);
	state.next_in = src;
	state.avail_in = srcSize;
	state.next_out = dst;
	state.crc_flag = ISAL_ZLIB;
	state.avail_out = decSize;
	int err = isal_inflate(&state);
	return (err == ISAL_DECOMP_OK);
}

#else
#error "expected library selection MACRO"
#endif

int fSize(FILE *f)
{
	int prev = ftell(f);
	fseek(f, 0L, SEEK_END);
	int sz = ftell(f);
	fseek(f, prev, SEEK_SET);
	return sz;
}

bool validPNGSignature(FILE *in)
{
	char buffer[8];
	if (fread(&buffer[0], sizeof(char), 8, in) != 8)
	{
		return false;
	}
	return (strncmp(&buffer[0], "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a", 8) == 0);
}

unsigned int swapUInt(const unsigned char *a)
{
	return (((unsigned int)a[0] << 24) | ((unsigned int)a[1] << 16) | ((unsigned int)a[2] << 8) | ((unsigned int)a[3]));
}

struct PNG_Chunk *readChunk(FILE *in)
{
	unsigned int tmp;
	if (fread(&tmp, sizeof(tmp), 1, in) < 1)
	{
		return NULL;
	}
	tmp = swapUInt((unsigned char *)&tmp);
	if (tmp < 0)
	{
		return NULL;
	}
	struct PNG_Chunk *ch = malloc(sizeof(*ch) + tmp);
	if (ch == NULL)
	{
		return NULL;
	}
	ch->length = tmp;
	if (fread(&(ch->type), sizeof(unsigned char), 4, in) < 4)
	{
		free(ch);
		return NULL;
	}
	if (fread(&(ch->data), sizeof(unsigned char), ch->length, in) < ch->length)
	{
		free(ch);
		return NULL;
	}
	if (fread(&(ch->crc), sizeof(unsigned int), 1, in) < 1)
	{
		free(ch);
		return NULL;
	}
	return ch;
}

bool isIHDRCorrect(struct PNG_Header *hdr)
{
	return (hdr->mustbe0 == 0);
}

bool isPNGSupported(struct PNG_Header *hdr)
{
	if (hdr->interlaced != 0)
	{
		return false;
	}

	if (hdr->depth != 8)
	{
		return false;
	}
	if ((hdr->colType != 0) && (hdr->colType != 2) && (hdr->colType != 3))
	{
		return false;
	}
	return true;
}

unsigned char paethPredictor(int a, int b, int c)
{
	int p = a + b - c;
	int pa = abs(p - a);
	int pb = abs(p - b);
	int pc = abs(p - c);
	if (pa <= pb && pa <= pc)
	{
		return a;
	}
	else if (pb <= pc)
	{
		return b;
	}
	else
	{
		return c;
	}
}

int filterTypes(unsigned char *res, unsigned int height, unsigned int width, unsigned int depth, bool indexed, unsigned char *palette, unsigned char *templine, int trueDepth, FILE *out)
{
	unsigned int scanLineLength = depth * width;
	unsigned char *temp = palette;
	for (int i = 0; i < height; ++i)
	{
		unsigned char filter = res[i * (scanLineLength + 1)];
		for (int j = 1; j <= scanLineLength; j++)
		{
			unsigned int number1 = (scanLineLength + 1) * i + j;
			unsigned int number2 = (scanLineLength + 1) * (i - 1) + j;
			temp += 1;
			if (filter == 1 && j > depth)
			{
				res[number1] += res[number1 - depth];
			}
			else if (filter == 2 && i > 0)
			{
				res[number1] += res[number2];
			}
			else if (filter == 3 && i > 0)
			{
				if (j > depth)
				{
					res[number1] += ((int)res[number2] + (int)res[number1 - depth]) / 2;
				}
				else
				{
					res[number1] += ((int)res[number2]) / 2;
				}
			}
			else if (filter == 4)
			{
				if (i > 0 && j > depth)
				{
					res[number1] += paethPredictor(res[number1 - depth], res[number2], res[number2 - depth]);
				}
				else if (i > 0)
				{
					res[number1] += res[number2];
				}
				else
				{
					res[number1] += res[number1 - depth];
				}
			}
			else if (filter != 0 && filter != 1 && filter != 2 && filter != 3 && filter != 4)
			{
				return -1;
			}
		}
		if (indexed)
		{
			unsigned char *dst = templine;
			unsigned char *src = &res[(i * (scanLineLength + 1)) + 1];
			for (int j = 0; j < width; j++)
			{
				memcpy(dst, &palette[src[0] * 3], trueDepth);
				dst += trueDepth;
				src++;
			}
			fwrite(templine, sizeof(unsigned char), width * trueDepth, out);
		}
		else
		{
			fwrite(res + (i * (scanLineLength + 1)) + 1, sizeof(unsigned char), scanLineLength, out);
		}
	}
	return 0;
}

void composePNMHeader(FILE *out, unsigned int width, unsigned int height, unsigned int depth)
{
	char ct = '5';
	if (depth == 3)
	{
		ct = '6';
	}
	fprintf(out, "P%c\n%d %d\n255\n", ct, width, height);
}

bool isPaletteGrayscale(const unsigned char *pal, unsigned int palLength)
{
	bool isGS = true;
	int cp = 0;
	for (int i = 0; (i < palLength) && isGS; i++)
	{
		isGS = ((pal[cp] == pal[cp + 1]) && (pal[cp] == pal[cp + 2]));
		cp += 3;
	}
	return isGS;
}

int main(int argc, char *argv[])
{
	if (argc < 3)
	{
		fprintf(stderr, "not enough args");
		return ERROR_PARAMETER_INVALID;
	}
	FILE *in = fopen(argv[1], "rb");
	if (!in)
	{
		fprintf(stderr, "file not found");
		return ERROR_CANNOT_OPEN_FILE;
	}

	if (!validPNGSignature(in))
	{
		fclose(in);
		fprintf(stderr, "bad format");
		return ERROR_DATA_INVALID;
	}

	struct PNG_Chunk *IHDRChunk = readChunk(in);
	if (IHDRChunk == NULL)
	{
		fclose(in);
		fprintf(stderr, "expected PNG header chunk");
		return ERROR_DATA_INVALID;
	}

	if (strncmp(IHDRChunk->type, "IHDR", 4) != 0 || (IHDRChunk->length != 13))
	{
		fclose(in);
		free(IHDRChunk);
		fprintf(stderr, "unsupported  PNG format");
		return ERROR_DATA_INVALID;
	}

	struct PNG_Header hdr;
	memcpy((unsigned char *)&hdr, IHDRChunk->data, sizeof(hdr));
	hdr.width = swapUInt((unsigned char *)&hdr.width);
	hdr.height = swapUInt((unsigned char *)&hdr.height);
	free(IHDRChunk);

	if (!isIHDRCorrect(&hdr))
	{
		fclose(in);
		fprintf(stderr, "unsupported PNG format");
		return ERROR_DATA_INVALID;
	}

	if (!isPNGSupported(&hdr))
	{
		fclose(in);
		fprintf(stderr, "unsupported PNG format");
		return ERROR_UNSUPPORTED;
	}

	bool palFound = false;
	unsigned char *palette = NULL;
	unsigned int palLength = 0;
	int fullfs = fSize(in);
	unsigned char *data = malloc(fullfs);
	unsigned char *cp = data;
	unsigned int trueSize = 0;
	if (data == NULL)
	{
		fclose(in);
		fprintf(stderr, "out of memory");
		return ERROR_OUT_OF_MEMORY;
	}

	bool eof = false;
	struct PNG_Chunk *PNG_Chunk = NULL;

	do
	{
		free(PNG_Chunk);
		PNG_Chunk = readChunk(in);
		if (PNG_Chunk == NULL)
		{
			fclose(in);
			free(data);
			free(palette);
			fprintf(stderr, "invalid data");
			return ERROR_DATA_INVALID;
		}

		if (!strncmp(PNG_Chunk->type, "PLTE", 4))
		{
			if ((PNG_Chunk->length % 3) != 0)
			{
				fclose(in);
				free(data);
				fprintf(stderr, "invalid PLTE сhunk");
				return ERROR_DATA_INVALID;
			}
			palette = malloc(PNG_Chunk->length);
			if (palette == NULL)
			{
				fclose(in);
				free(data);
				fprintf(stderr, "not enough memory");
				return ERROR_OUT_OF_MEMORY;
			}
			memcpy(palette, PNG_Chunk->data, PNG_Chunk->length);
			palLength = PNG_Chunk->length / 3;
			palFound = true;
			continue;
		}
		if (!strncmp(PNG_Chunk->type, "IDAT", 4))
		{
			if (PNG_Chunk->length > 0)
			{
				memcpy(cp, PNG_Chunk->data, PNG_Chunk->length);
				cp += PNG_Chunk->length;
				trueSize += PNG_Chunk->length;
			}
			continue;
		}
		/*		if (!strncmp(PNG_Chunk->type, "tRNS", 4) || !strncmp(PNG_Chunk->type, "gAMA", 4) ||
					!strncmp(PNG_Chunk->type, "cHRM", 4) || !strncmp(PNG_Chunk->type, "sRGB", 4) ||
		   !strncmp(PNG_Chunk->type, "iCCP", 4))
				{
					fclose(in);
					free(data);
					free(palette);
					fprintf(stderr, "unsupported chunk type in source file");
					return ERROR_UNSUPPORTED;
				}*/

		eof = (strncmp(PNG_Chunk->type, "IEND", 4) == 0);
	} while (!eof);
	free(PNG_Chunk);
	fclose(in);

	if (((hdr.colType == 0) && palFound) || ((hdr.colType == 3) && !palFound))
	{
		fprintf(stderr, "image not comply to header");
		free(data);
		free(palette);
		return ERROR_DATA_INVALID;
	}

	int trueDepth = 3;
	int srcDepth = 3;
	if (hdr.colType == 0)
	{
		trueDepth = 1;
	}
	if ((hdr.colType == 3) && (isPaletteGrayscale(palette, palLength)))
	{
		trueDepth = 1;
	}
	if (hdr.colType != 2)
	{
		srcDepth = 1;
	}
	bool indexed = (hdr.colType == 3);

	size_t decompressSize = hdr.height * (hdr.width * srcDepth + 1);
	unsigned char *unpacked = malloc(decompressSize);
	if (unpacked == NULL)
	{
		free(data);
		free(palette);
		fprintf(stderr, "not enough memory");
		return ERROR_OUT_OF_MEMORY;
	}

	if (trueSize == 0 || !decompress(data, unpacked, trueSize, decompressSize))
	{
		free(unpacked);
		free(palette);
		free(data);
		fprintf(stderr, "deflation error");
		return ERROR_DATA_INVALID;
	}

	free(data);

	unsigned char *tmpl = NULL;
	if (indexed)
	{
		tmpl = malloc(hdr.width * trueDepth);
		if (tmpl == NULL)
		{
			free(unpacked);
			free(palette);
			fprintf(stderr, "not enough memory");
			return ERROR_OUT_OF_MEMORY;
		}
	}

	FILE *out = fopen(argv[2], "wb");
	if (!out)
	{
		fclose(in);
		free(palette);
		return ERROR_CANNOT_OPEN_FILE;
	}

	composePNMHeader(out, hdr.width, hdr.height, trueDepth);

	if (filterTypes(unpacked, hdr.height, hdr.width, srcDepth, indexed, palette, tmpl, trueDepth, out) != 0)
	{
		fclose(out);
		free(unpacked);
		free(palette);
		free(tmpl);
		return ERROR_UNSUPPORTED;
	}

	fclose(out);
	free(unpacked);
	free(palette);
	free(tmpl);
	return SUCCESS;
}
