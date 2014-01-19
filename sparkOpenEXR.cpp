/* sparkOpenEXR, a .exr file reader for Flame and Smoke
   Written by lewissaunders@myrealbox.com, March 2006

   Caveat: it will load an entire directory full of files
   as a sequence.  If you want a still, put the file in a
   directory on its own.

   Building from source might be a bit of a mission.  You'll
   need the OpenEXR headers and libraries from openexr.org, just
   get the latest source, build it and install it.

   On Irix, you will need the MIPSPro C++ compiler. GCC will
   not work, since Flame itself is compiled with MIPSPro and
   the C++ ABIs are not the same. Or whatever. It doesn't work.

   You can hack up the existing Sparks Makefile, or just start
   over.  For a 64-bit build on Irix I used something like:

   CC -64 -mips4 -O3 -I/usr/local/include/OpenEXR -c
	./sparkOpenEXR.cpp -o ./sparkOpenEXR.o

   then:

   ld -s -64 -shared -check_registry /usr/lib/so_locations
	./sparkOpenEXR.o -L/usr/local/lib64 -lgen -B static -lIlmImf
	-lImath	-lHalf -lIex -o ./sparkOpenEXR.spark_64

   Best of luck.  If you find any rough edges, do get in touch.

   Oh, and, yeah, the following source is really, like, disgusting,
   Mixing a C++ library API and a C plugin API... ew. */

#include <stdlib.h>
#include <stdio.h>
#include <libgen.h>
#include <dirent.h>
#include "spark.h"
#include <half.h>
#include <ImfInputFile.h>
#include <ImfChannelList.h>
#include <ImfArray.h>
#include <ImfVersion.h>

/* Spark pixel packing structs */
typedef RGB8_OGL  px8_t;
typedef RGB12_OGL px12_t;

/* Forward declarations */
unsigned long *openPush(int callbackArg, SparkInfoStruct sparkInfo);
unsigned long *setupTouch(int callbackArg, SparkInfoStruct sparkInfo);
unsigned long *pupTouch(int callbackArg, SparkInfoStruct sparkInfo);
unsigned long *startTouch(int callbackArg, SparkInfoStruct sparkInfo);
unsigned long *endTouch(int callbackArg, SparkInfoStruct sparkInfo);
unsigned long *touch(int callbackArg, SparkInfoStruct sparkInfo);

void fileAdd(char *f);
void freeCache(void);
void raze(void);

/* Global state variables */
int		pushedOpen = 0;
int		cacheValid = 0;
int		cacheFile = 0;

/* Pixel cache */
void		*rCache = NULL;
void		*gCache = NULL;
void		*bCache = NULL;

Imf::PixelType	rType;
Imf::PixelType	gType;
Imf::PixelType	bType;

/* File path string control */
SparkStringStruct SparkSetupString1 = {
	"",
	"Path (don't touch!): %s",
	SPARK_FLAG_NO_ANIM | SPARK_FLAG_NO_INPUT,
	setupTouch
};

/* Open button */
SparkPushStruct SparkPush6 = {
	"Open...",
	openPush
};

/* Start file control */
SparkIntStruct SparkInt7 = {
	1,
	1,
	10000,
	1,
	SPARK_FLAG_NO_ANIM,
	"Start file: %d",
	startTouch
};

/* End file control */
SparkIntStruct SparkInt14 = {
	1,
	1,
	10000,
	1,
	SPARK_FLAG_NO_ANIM,
	"End file: %d",
	endTouch
};

/* Slip field */
SparkIntStruct SparkInt13 = {
	0,
	-10000,
	10000,
	1,
	0,
	"Slip: %d frames",
	touch
};

/* Regen toggle */
SparkBooleanStruct SparkBoolean12 = {
	0,
	"Regen",
	NULL
};

/* Red channel selector */
SparkPupStruct SparkPup16 = {
	0,
	0,
	pupTouch,
	NULL
};

/* Green channel selector */
SparkPupStruct SparkPup17 = {
	0,
	0,
	pupTouch,
	NULL
};

/* Blue channel selector */
SparkPupStruct SparkPup18 = {
	0,
	0,
	pupTouch,
	NULL
};

/* Master Exposure */
SparkFloatStruct SparkFloat22 = {
	0.0,
	-20.0,
	20.0,
	0.1,
	NULL,
	"RGB Exposure: %.1f stops",
	touch
};

/* Red Exposure */
SparkFloatStruct SparkFloat23 = {
	0.0,
	-20.0,
	20.0,
	0.1,
	NULL,
	"R Exposure: %.1f stops",
	touch
};

/* Green Exposure */
SparkFloatStruct SparkFloat24 = {
	0.0,
	-20.0,
	20.0,
	0.1,
	NULL,
	"G Exposure: %.1f stops",
	touch
};

/* Blue Exposure */
SparkFloatStruct SparkFloat25 = {
	0.0,
	-20.0,
	20.0,
	0.1,
	NULL,
	"B Exposure: %.1f stops",
	touch
};

/* Master Gamma */
SparkFloatStruct SparkFloat29 = {
	2.2,
	0.0,
	5.0,
	0.01,
	NULL,
	"RGB Gamma: %.2f",
	touch
};

/* Red Gamma */
SparkFloatStruct SparkFloat30 = {
	1.0,
	0.0,
	5.0,
	0.01,
	NULL,
	"R Gamma: %.2f",
	touch
};

/* Green Gamma */
SparkFloatStruct SparkFloat31 = {
	1.0,
	0.0,
	5.0,
	0.01,
	NULL,
	"G Gamma: %.2f",
	touch
};

/* Blue Gamma */
SparkFloatStruct SparkFloat32 = {
	1.0,
	0.0,
	5.0,
	0.01,
	NULL,
	"B Gamma: %.2f",
	touch
};

/* ============================== Callbacks ===================================== */

/* Touch rewire popups */
unsigned long *pupTouch(int callbackArg, SparkInfoStruct sparkInfo) {
	freeCache();
	return(touch(callbackArg, sparkInfo));
}

/* Touch start control */
unsigned long *startTouch(int callbackArg, SparkInfoStruct sparkInfo) {
	if(SparkInt7.Value > SparkInt14.Value) {
		SparkInt7.Value = SparkInt14.Value;
	}
	return(touch(callbackArg, sparkInfo));
}

/* Touch end control */
unsigned long *endTouch(int callbackArg, SparkInfoStruct sparkInfo) {
	if(SparkInt14.Value < SparkInt7.Value) {
		SparkInt14.Value = SparkInt7.Value;
	}
	return(touch(callbackArg, sparkInfo));
}

/* Touch any other control */
unsigned long *touch(int callbackArg, SparkInfoStruct sparkInfo) {
	unsigned long		*r;

	/* Processing */
	if(SparkBoolean12.Value) {
		r = SparkProcess(sparkInfo);
		sparkViewingDraw();
		return(r);
	} else {
		return(NULL);
	}
}

/* Open button push callback */
unsigned long *openPush(int callbackArg, SparkInfoStruct sparkInfo) {
	printf("sparkOpenEXR: in openPush()\n");
	pushedOpen = 1;
	sparkFileBrowserDisplayLoad("/", "exr", fileAdd);
	return(NULL);
}

/* Setup control modification callback - seems controls can't be set truly read-only */
unsigned long *setupTouch(int callbackArg, SparkInfoStruct sparkInfo) {
	sparkMessageConfirm("Really, don't touch that!  You'll need to hit Open... again now.");
	raze();
	return(NULL);
}

/* ============================== Functions ===================================== */

/* Checks OpenEXR magic bytes */
void checkMagic(const char fileName[]) {
	/* printf("sparkOpenEXR: checking magic in %s\n", fileName); */
	std::ifstream	f(fileName, std::ios_base::binary);
	char 		bytes[4] = {0, 0, 0, 0};

	f.read(bytes, sizeof(bytes));
	if(!Imf::isImfMagic(bytes)) {
		printf("sparkOpenEXR: No OpenEXR magic bytes in file %s\n", fileName);
		sparkError("No OpenEXR magic bytes in file!");
	}
}

/* For qsort() */
int compare(const void *a, const void *b) {
	char 		*aa, *bb;

	aa = *((char**)a);
	bb = *((char**)b);

	return(strcmp(aa, bb));
}

/* Free and reset global pixel cache */
void freeCache(void) {
	cacheValid = 0;

	/* Sometimes cache pointers are the same... */
	if(bCache == gCache || bCache == rCache) {
		bCache = NULL;
	}
	if(gCache == rCache) {
		gCache = NULL;
	}

	if(rCache != NULL) {
		free(rCache);
		rCache = NULL;
	}
	if(gCache != NULL) {
		free(gCache);
		gCache = NULL;
	}
	if(bCache != NULL) {
		free(bCache);
		bCache = NULL;
	}
}

/* Reset global variables */
void raze(void) {
	int		i;

	printf("sparkOpenEXR: in raze()\n");

	freeCache();

	SparkSetupString1.Value[0] = 0;

	SparkFloat22.Value = 0.0;
	SparkFloat29.Value = 2.2;

	for(i = 0; i < SparkPup16.TitleCount; i++) {
		free(SparkPup16.Titles[i]);
	}
	SparkPup16.TitleCount = 0;
	SparkPup16.Value = 0;
	SparkFloat23.Value = 0.0;
	SparkFloat30.Value = 1.0;
	
	for(i = 0; i < SparkPup17.TitleCount; i++) {
		free(SparkPup17.Titles[i]);
	}
	SparkPup17.TitleCount = 0;
	SparkPup17.Value = 0;
	SparkFloat24.Value = 0.0;
	SparkFloat31.Value = 1.0;

	for(i = 0; i < SparkPup18.TitleCount; i++) {
		free(SparkPup18.Titles[i]);
	}
	SparkPup18.TitleCount = 0;
	SparkPup18.Value = 0;
	SparkFloat25.Value = 0.0;
	SparkFloat32.Value = 1.0;
}

/* Set up interface with values from OpenEXR header */
void readHeader(void) {
	int		channelCount = 0;
	char		*s;

	printf("sparkOpenEXR: in readHeader() for %s\n", SparkSetupString1.Value);
	checkMagic(SparkSetupString1.Value);
	Imf::InputFile 	inputFile2(SparkSetupString1.Value);
	const Imf::ChannelList &channels = inputFile2.header().channels();
	for(Imf::ChannelList::ConstIterator i = channels.begin(); i != channels.end(); i++) {
		channelCount++;
		if(channelCount >= 20) {
			sparkMessage("Limit of 20 channels reached :-(");
			break;
		}
		s = strdup(i.name());
		SparkPup16.Titles[SparkPup16.TitleCount] = (char *)malloc(strlen("R <- ") + strlen(s) + 1);
		strcpy(SparkPup16.Titles[SparkPup16.TitleCount], "R <- ");
		strcat(SparkPup16.Titles[SparkPup16.TitleCount], s);
		SparkPup16.TitleCount++;
		sparkControlUpdate(8);
		SparkPup17.Titles[SparkPup17.TitleCount] = (char *)malloc(strlen("G <- ") + strlen(s) + 1);
		strcpy(SparkPup17.Titles[SparkPup17.TitleCount], "G <- ");
		strcat(SparkPup17.Titles[SparkPup17.TitleCount], s);
		SparkPup17.TitleCount++;
		sparkControlUpdate(9);
		SparkPup18.Titles[SparkPup18.TitleCount] = (char *)malloc(strlen("B <- ") + strlen(s) + 1);
		strcpy(SparkPup18.Titles[SparkPup18.TitleCount], "B <- ");
		strcat(SparkPup18.Titles[SparkPup18.TitleCount], s);
		SparkPup18.TitleCount++;
		sparkControlUpdate(10);
		free(s);
	}
}

void fileAdd(char *f) {
	char		*s, *folderName;
	int		fileCount = 0;
	DIR		*dir;
	struct dirent	*dirEntry;

	if(pushedOpen) {
		/* First call after open pushed */
		raze();
	}

	printf("sparkOpenEXR: in fileAdd(): adding file %s\n", f);

	if(strlen(f) >= SPARK_MAX_STRING_LENGTH) {
		sparkError("Path too long! SPARK_MAX_STRING_LENGTH character limit :-(");
	}
	
	strcpy(SparkSetupString1.Value, f);
	sparkControlUpdate(1);

	/* Count files in directory and update end control */
	s = strdup(SparkSetupString1.Value);
	folderName = strdup(dirname(s));
	dir = opendir(folderName);
	while(dirEntry = readdir(dir)) {
		if(strlen(dirEntry->d_name) < 5 || strcmp(&dirEntry->d_name[strlen(dirEntry->d_name) - 4], ".exr") != 0) {
			continue;
		} else {
			fileCount++;
		}
	}
	closedir(dir);
	free(s);
	free(folderName);

	SparkInt14.Value = fileCount;
		

	if(pushedOpen) {
		/* We now have the first filename */
		readHeader();
	
		if(SparkBoolean12.Value) {
			/* Regen is pressed, read first frame */
			sparkReprocess();
		}
	
		pushedOpen = 0;
	}
}

/* Return pointer to string containing channel name */
char *getChannel(char c) {
	switch(c) {
		case 'r':
			return(&(SparkPup16.Titles[SparkPup16.Value][5]));
		break;
		case 'g':
			return(&(SparkPup17.Titles[SparkPup17.Value][5]));
		break;
		case 'b':
			return(&(SparkPup18.Titles[SparkPup18.Value][5]));
		break;
	}
	return(NULL);
}

/* Read image, output into resultBuffer */
void readImage(SparkInfoStruct *sparkInfo, SparkMemBufStruct *resultBuffer) {
	int		fileNo;
	int		slip = SparkInt13.Value;
	int		start = SparkInt7.Value - 1;
	int		end = SparkInt14.Value - 1;
	char		*file, *s, *fileName, *folderName;
	char		**files = NULL;
	int		fileCount = 0;
	DIR		*dir;
	struct dirent	*dirEntry;

	/* Figure out which file to display */
	fileNo = sparkInfo->FrameNo + slip + start;
	if(fileNo < start) {
		fileNo = start;
	}
	if(fileNo > end) {
		fileNo = end;
	}

	printf("sparkOpenEXR: in doRead(): frame %d, file %d = \n", sparkInfo->FrameNo, fileNo);

	if(cacheValid == 0 || cacheFile != fileNo) {
		/* Get name of nth file */
		s = strdup(SparkSetupString1.Value);
		folderName = strdup(dirname(s));
		dir = opendir(folderName);
		while(dirEntry = readdir(dir)) {
			if(strlen(dirEntry->d_name) < 5 || strcmp(&dirEntry->d_name[strlen(dirEntry->d_name) - 4], ".exr") != 0) {
				continue;
			} else {
				fileCount++;
				files = (char **)realloc(files, fileCount * sizeof(char *));
				files[fileCount - 1] = strdup(dirEntry->d_name);
			}
		}
		closedir(dir);

		qsort(files, fileCount, sizeof(char *), compare);

		if(fileNo > fileCount - 1) {
			fileNo = fileCount - 1;
		}

		fileName = strdup(files[fileNo]);
		file = (char *)malloc(strlen(folderName) + strlen("/") + strlen(fileName) + 1);
		strcpy(file, folderName);
		strcat(file, "/");
		strcat(file, fileName);

		printf("\t%s\n\n", file);

		/*for(int i = 0; i < fileCount; i++) {
			printf("sparkOpenEXR: I have file %d, %s\n", i, files[i]);
		}*/

		/* And now the actual OpenEXR stuff */
		checkMagic(file);

		Imf::InputFile 		inputFile(file);
		Imf::Channel		chan;
		Imf::FrameBuffer	fb;
		Imath::Box2i 		dw = inputFile.header().dataWindow();
		int 			width = dw.max.x - dw.min.x + 1;
		int 			height = dw.max.y - dw.min.y + 1;
	
		if(width != sparkInfo->FrameWidth || height != sparkInfo->FrameHeight) {
			char 	*s = (char *)malloc(256);	/* Yeah, fixed buffer size, screw it... */
			
			sprintf(s, "Wrong size: file is %d x %d, adjust Spark output size to match!", width, height);
			sparkError(s);
			free(s);
		}

		/* Read pixels from file into cache */
		/* Set up channel mapped to R */
		chan = inputFile.header().channels()[getChannel('r')];
		switch(chan.type) {
			case Imf::HALF:
				rCache = malloc(width * height * sizeof(half));
				rType = Imf::HALF;
				fb.insert(getChannel('r'), Imf::Slice(Imf::HALF, (char *)rCache - dw.min.x - dw.min.y * width,
				          sizeof(half) * chan.xSampling, sizeof(half) * chan.ySampling * width,
				          chan.xSampling,
				          chan.ySampling, 0.0));
			break;
			case Imf::FLOAT:
				rCache = malloc(width * height * sizeof(float));
				rType = Imf::FLOAT;
				fb.insert(getChannel('r'), Imf::Slice(Imf::FLOAT, (char *)rCache - dw.min.x - dw.min.y * width,
				          sizeof(float) * chan.xSampling, sizeof(float) * chan.ySampling * width,
				          chan.xSampling,
				          chan.ySampling, 0.0));
			break;
			case Imf::UINT:
				rCache = malloc(width * height * sizeof(uint32_t));
				rType = Imf::UINT;
				fb.insert(getChannel('r'), Imf::Slice(Imf::UINT, (char *)rCache - dw.min.x - dw.min.y * width,
				          sizeof(uint32_t) * chan.xSampling, sizeof(uint32_t) * chan.ySampling * width,
				          chan.xSampling,
				          chan.ySampling, 0.0));
			break;
		}
		
		/* Set up channel mapped to G */
		/* If same as R, merely set pointer */
		if(strcmp(getChannel('g'), getChannel('r')) == 0) {
			gCache = rCache;
			gType = rType;
		} else {
			chan = inputFile.header().channels()[getChannel('g')];
			switch(chan.type) {
				case Imf::HALF:
					gCache = malloc(width * height * sizeof(half));
					gType = Imf::HALF;
					fb.insert(getChannel('g'), Imf::Slice(Imf::HALF, (char *)gCache - dw.min.x - dw.min.y * width,
				        	  sizeof(half) * chan.xSampling, sizeof(half) * chan.ySampling * width,
				        	  chan.xSampling,
				        	  chan.ySampling, 0.0));
				break;
				case Imf::FLOAT:
					gCache = malloc(width * height * sizeof(float));
					gType = Imf::FLOAT;
					fb.insert(getChannel('g'), Imf::Slice(Imf::FLOAT, (char *)gCache - dw.min.x - dw.min.y * width,
				        	  sizeof(float) * chan.xSampling, sizeof(float) * chan.ySampling * width,
				        	  chan.xSampling,
				        	  chan.ySampling, 0.0));
				break;
				case Imf::UINT:
					gCache = malloc(width * height * sizeof(uint32_t));
					gType = Imf::UINT;
					fb.insert(getChannel('g'), Imf::Slice(Imf::UINT, (char *)gCache - dw.min.x - dw.min.y * width,
				        	  sizeof(uint32_t) * chan.xSampling, sizeof(uint32_t) * chan.ySampling * width,
				        	  chan.xSampling,
				        	  chan.ySampling, 0.0));
				break;
			}
		}

		/* Set up channel mapped to B */
		/* If same as R or G, merely set pointer */
		if(strcmp(getChannel('b'), getChannel('r')) == 0) {
			bCache = rCache;
			bType = rType;
		} else if(strcmp(getChannel('b'), getChannel('g')) == 0) {
			bCache = gCache;
			bType = gType;
		} else {
			chan = inputFile.header().channels()[getChannel('b')];
			switch(chan.type) {
				case Imf::HALF:
					bCache = malloc(width * height * sizeof(half));
					bType = Imf::HALF;
					fb.insert(getChannel('b'), Imf::Slice(Imf::HALF, (char *)bCache - dw.min.x - dw.min.y * width,
				        	  sizeof(half) * chan.xSampling, sizeof(half) * chan.ySampling * width,
				        	  chan.xSampling,
				        	  chan.ySampling, 0.0));
				break;
				case Imf::FLOAT:
					bCache = malloc(width * height * sizeof(float));
					bType = Imf::FLOAT;
					fb.insert(getChannel('b'), Imf::Slice(Imf::FLOAT, (char *)bCache - dw.min.x - dw.min.y * width,
				        	  sizeof(float) * chan.xSampling, sizeof(float) * chan.ySampling * width,
				        	  chan.xSampling,
				        	  chan.ySampling, 0.0));
				break;
				case Imf::UINT:
					bCache = malloc(width * height * sizeof(uint32_t));
					bType = Imf::UINT;
					fb.insert(getChannel('b'), Imf::Slice(Imf::UINT, (char *)bCache - dw.min.x - dw.min.y * width,
				        	  sizeof(uint32_t) * chan.xSampling, sizeof(uint32_t) * chan.ySampling * width,
				        	  chan.xSampling,
				        	  chan.ySampling, 0.0));
				break;
			}
		}

		inputFile.setFrameBuffer(fb);
		inputFile.readPixels(dw.min.y, dw.max.y);

		cacheValid = 1;
		cacheFile = fileNo;

		/* Free from everything. (hee hee hee) */
		for(int i = 0; i < fileCount; i++) {
			free(files[i]);
		}
		free(files);
		free(file);
		free(s);
		free(folderName);
		free(fileName);
	}

	/* Convert cached pixels and output to Spark buffer */
	for(int y = 0; y < resultBuffer->BufHeight; y++) {
		for(int x = 0; x < resultBuffer->BufWidth; x++) {
			/* Whatever to float */
			float		rgb[3];
			Imf::PixelType	*type[3] = {&rType, &gType, &bType};
			void		*cache[3] = {&rCache, &gCache, &bCache};
			float		*exposure[3] = {&SparkFloat23.Value, &SparkFloat24.Value, &SparkFloat25.Value};
			float		*gamma[3] = {&SparkFloat30.Value, &SparkFloat31.Value, &SparkFloat32.Value};
			float		max = resultBuffer->BufDepth == SPARKBUF_RGB_48 ? 4095.0 : 255.0;

			for(int i = 0; i < 3; i++) {
				switch(*(type[i])) {
					case Imf::HALF:
						rgb[i] = ((half *)(*(half **)(cache[i])))[(resultBuffer->BufHeight - y - 1) * resultBuffer->BufWidth + x];
						rgb[i] *= Imath::Math<float>::pow(2, *(exposure[i]) + SparkFloat22.Value + 2.47393);
						rgb[i] = std::max(0.0f, rgb[i]);
						rgb[i] = Imath::Math<float>::pow(rgb[i], 1.0 / (*gamma[i] * SparkFloat29.Value));
						rgb[i] *= 0.332 * max;
						if(rgb[i] > max) {
							rgb[i] = max;
						} else if(rgb[i] < 0.0) {
							rgb[i] = 0.0;
						}
					break;
					case Imf::FLOAT:
						rgb[i] = ((float *)(*(float **)(cache[i])))[(resultBuffer->BufHeight - y - 1) * resultBuffer->BufWidth + x];
						rgb[i] *= Imath::Math<float>::pow(2, *(exposure[i]) + SparkFloat22.Value + 2.47393);
						rgb[i] = std::max(0.0f, rgb[i]);
						rgb[i] = Imath::Math<float>::pow(rgb[i], 1.0 / (*gamma[i] * SparkFloat29.Value));
						rgb[i] *= 0.332 * max;
						if(rgb[i] > max) {
							rgb[i] = max;
						} else if(rgb[i] < 0.0) {
							rgb[i] = 0.0;
						}
					break;
					case Imf::UINT:
						rgb[i] = ((uint32_t *)(*(uint32_t **)(cache[i])))[(resultBuffer->BufHeight - y - 1) * resultBuffer->BufWidth + x];
					break;
				}
			}

			/* Float to 8/12 bit */
			if(resultBuffer->BufDepth == SPARKBUF_RGB_48) {
				void *pixel = (void *)&((char *)resultBuffer->Buffer)[y * resultBuffer->Stride + x * resultBuffer->Inc];
				((px12_t *)pixel)->r = rgb[0];
				((px12_t *)pixel)->g = rgb[1];
				((px12_t *)pixel)->b = rgb[2];
			} else {
				void *pixel = (void *)&((char *)resultBuffer->Buffer)[y * resultBuffer->Stride + x * resultBuffer->Inc];
				((px8_t *)pixel)->r = rgb[0];
				((px8_t *)pixel)->g = rgb[1];
				((px8_t *)pixel)->b = rgb[2];
			}
		}
	}
}

/* ============================== Sparks API ==================================== */

/* Return number of input clips required */
int SparkClips(void) {
	return(0);
}

/* New memory allocation interface - defining this keeps Batch happy */
void SparkMemoryTempBuffers(void) {
}

/* We want to run at the module level, not on the desktop */
unsigned int SparkInitialise(SparkInfoStruct sparkInfo) {
	printf("sparkOpenEXR: in SparkInitialise()\n");
	raze();
	Imf::staticInitialize();
	return(SPARK_MODULE);
}

/* The real work */
unsigned long *SparkProcess(SparkInfoStruct sparkInfo) {
	SparkMemBufStruct 	resultBuffer;

	printf("sparkOpenEXR: in SparkProcess(): frame %d\n", sparkInfo.FrameNo);

	/* Check result buffer is locked */
	if(!sparkMemGetBuffer(1, &resultBuffer)){
		return(NULL);
	}
	if(!(resultBuffer.BufState & MEMBUF_LOCKED)) {
		return(NULL);
	}
	
	if(SparkSetupString1.Value[0] != '/') {
		/* No input files yet - return black */
		memset(resultBuffer.Buffer, 0, (size_t)resultBuffer.BufSize);
	} else {
		if(SparkPup16.TitleCount == 0) {
			/* CRIKEY Bruce, the PUPS ate'n't POP-u-lated! */
			printf("sparkOpenEXR: in SparkProcess(): we have files but pups are not populated!\n");
			/* Flame returns strings saved in setups with \n on the end? Grr. */
			if(SparkSetupString1.Value[strlen(SparkSetupString1.Value) - 1] == '\n') {
				SparkSetupString1.Value[strlen(SparkSetupString1.Value) - 1] = 0;
			}
			readHeader();
		}
		readImage(&sparkInfo, &resultBuffer);
	}

	/* Return pointer to result buffer */
	return(resultBuffer.Buffer);
}

/* Uninit */
void SparkUnInitialise(SparkInfoStruct sparkInfo) {
	/* printf("sparkOpenEXR: in SparkUnInitialise()\n"); */
}
