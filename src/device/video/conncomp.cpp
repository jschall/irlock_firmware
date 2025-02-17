//
// begin license header
//
// This file is part of Pixy CMUcam5 or "Pixy" for short
//
// All Pixy source code is provided under the terms of the
// GNU General Public License v2 (http://www.gnu.org/licenses/gpl-2.0.html).
// Those wishing to use Pixy source code, software and/or
// technologies under different licensing terms should contact us at
// cmucam@cs.cmu.edu. Such licensing terms are available for
// all portions of the Pixy codebase presented here.
//
// end license header
//

#include <stdio.h>
#include <debug.h>
#include <string.h>
#include <math.h>
#include "pixy_init.h"
#include "camera.h"
#include "cameravals.h"
#include "param.h"
#include "conncomp.h"
#include "blobs.h"
#include "led.h"
#include "qqueue.h"


Qqueue *g_qqueue;
Blobs *g_blobs;

int g_loop = 0;

int32_t cc_servo(const uint32_t &start)
{
	g_loop = start;
	return 0;
}

static const ProcModule g_module[] =
{
	{
	"cc_getRLSFrame",
	(ProcPtr)cc_getRLSFrameChirp, 
	{END}, 
	"Get a frame of color run-length segments (RLS)"
	"@r 0 if success, negative if error"
	"@r CCQ1 formated data, including 8-palette"
	},
	{
	"cc_setSigRegion",
	(ProcPtr)cc_setSigRegion, 
	{CRP_UINT32, CRP_UINT8, CRP_HTYPE(FOURCC('R','E','G','1')), END}, 
	"Set signature by selecting region in image"
	"@p type 0=normal signature, 1=color code signature"
	"@p signature numerical index of signature, can be 1-7"
	"@p region user-selected region"
	"@r 0 to 100 if success where 100=good, 0=poor, negative if error"
	},
	{
	"cc_setSigPoint",
	(ProcPtr)cc_setSigPoint, 
	{CRP_UINT32, CRP_UINT8, CRP_HTYPE(FOURCC('P','N','T','1')), END}, 
	"Set signature by selecting point in image"
	"@p type 0=normal signature, 1=color code signature"
	"@p signature numerical index of signature, can be 1-7"
	"@p point user-selected point"
	"@r 0 to 100 if success where 100=good, 0=poor, negative if error"
	},
	{
	"cc_clearSig",
	(ProcPtr)cc_clearSig, 
	{CRP_UINT8, END}, 
	"Clear signature"
	"@p signature numerical index of signature, can be 1-7"
	"@r 0 if success, negative if error"
	},
	{
	"cc_clearAllSig",
	(ProcPtr)cc_clearAllSig, 
	{END}, 
	"Clear signature"
	"@r 0 if success, negative if error"
	},
	{
	"cc_setMemory",
	(ProcPtr)cc_setMemory,
	{CRP_UINT32, CRP_UINTS8, END},
	"" 
	},
	END
};

const uint32_t g_colors[] = 
{
	0xffffff, // 0 white
	0xff0000, // 1 red
	0xff8000, // 2 orange
	0xffff00, // 3 yellow
	0x00ff00, // 4 green
	0x00ffff, // 5 cyan
	0x0000ff, // 6 blue
	0xff00ff  // 7 violet
};

static ChirpProc g_getRLSFrameM0 = -1;

int cc_loadLut(void)
{
	int i, res;
	uint32_t len;
	char id[32];
	ColorModel *pmodel;

	// indicate that raw frame has been overwritten
	g_rawFrame.m_pixels = NULL;
	// clear lut
	g_blobs->m_clut->clear();

//MJLM Don't do this checking, but just load pmodel 1, which is the hard-coded luminance model
//	for (i=1; i<=NUM_MODELS; i++)
//	{
//		sprintf(id, "signature%d", i);
//		// get signature and add to color lut
//		res = prm_get(id, &len, &pmodel, END);
//		if (res<0)
//			return res;
//		g_blobs->m_clut->add(pmodel, i);
//	}
	g_blobs->m_clut->add(pmodel, i);
	
	// go ahead and flush since we've changed things
	g_qqueue->flush();

	return 0;
}

void cc_loadParams(void)
{
	int i;
	ColorModel model;
	char id[32], desc[32];

	// set up signatures, load later
	for (i=1; i<=NUM_MODELS; i++)
	{
		sprintf(id, "signature%d", i);
		sprintf(desc, "Color signature %d", i);
		// add if it doesn't exist yet
		prm_add(id, PRM_FLAG_INTERNAL, desc, INTS8(sizeof(ColorModel), &model), END);
	}

	// others -----

	// setup
	prm_add("Max blocks", 0, 
		"Sets the maximum total blocks sent per frame. (default 1000)", UINT16(1000), END);
	prm_add("Max blocks per signature", 0, 
		"Sets the maximum blocks for each color signature sent for each frame. (default 1000)", UINT16(1000), END);
	prm_add("Min block area", 0, 
	"Sets the minimum required area in pixels for a block.  Blocks with less area won't be sent. (default 20)", UINT32(1), END); //TMS: default 20
	prm_add("Min saturation", 0,
		"@c Signature_creation Sets the minimum allowed color saturation for when generating color signatures. Applies during teaching. (default 15.0)", FLT32(15.0), END);
	prm_add("Hue spread", 0,
		"@c Signature_creation Sets how inclusive the color signatures are with respect to hue. Applies during teaching. (default 1.5)", FLT32(1.5), END);
	prm_add("Saturation spread", 0,
		"@c Signature_creation Sets how inclusive the color signatures are with respect to saturation. Applies during teaching. (default 1.5)", FLT32(1.5), END);
	prm_add("CC min saturation", 0,
		"@c Signature_creation Sets the minimum allowed color saturation for when generating color code (CC) signatures. Applies during teaching. (default 15.0)", FLT32(15.0), END);
	prm_add("CC hue spread", 0,
		"@c Signature_creation Sets how inclusive the color code (CC) signatures are with respect to hue. Applies during teaching. (default 3.0)", FLT32(3.0), END);
	prm_add("CC saturation spread", 0,
		"@c Signature_creation Sets how inclusive the color code (CC) signatures are with respect to saturation for color codes. Applies during teaching. (default 50.0)", FLT32(50.0), END);
	prm_add("Color code mode", 0,
		"Sets the color code mode, 0=disabled, 1=enabled, 2=color codes only, 3=mixed (default 1)", INT8(1), END);

	// load
	uint8_t ccMode;
	uint16_t maxBlobs, maxBlobsPerModel;
	uint32_t minArea;

	prm_get("Max blocks", &maxBlobs, END);
	prm_get("Max blocks per signature", &maxBlobsPerModel, END);
	prm_get("Min block area", &minArea, END);
	prm_get("Color code mode", &ccMode, END);
	g_blobs->setParams(maxBlobs, maxBlobsPerModel, minArea, (ColorCodeMode)ccMode);

	cc_loadLut();

}

int cc_init(Chirp *chirp)
{
	g_qqueue = new Qqueue;
	g_blobs = new Blobs(g_qqueue);

	chirp->registerModule(g_module);	

	g_getRLSFrameM0 = g_chirpM0->getProc("getRLSFrame", NULL);

	if (g_getRLSFrameM0<0)
		return -1;

	cc_loadParams(); // setup default vals and load parameters

	return 0;
}

void cc_setBounds(const uint8_t mode)
{
	float minSat, hueTol, satTol;

	if (mode==1)
	{
		prm_get("CC min saturation", &minSat, END);
		prm_get("CC hue spread", &hueTol, END);
		prm_get("CC saturation spread", &satTol, END);
	}
	else
	{
		prm_get("Min saturation", &minSat, END);
		prm_get("Hue spread", &hueTol, END);
		prm_get("Saturation spread", &satTol, END);
	}

   	g_blobs->m_clut->setBounds(minSat, hueTol, satTol); 
}

// this routine assumes it can grab valid pixels in video memory described by the box
int32_t cc_setSigRegion(const uint32_t &type, const uint8_t &model, const uint16_t &xoffset, const uint16_t &yoffset, const uint16_t &width, const uint16_t &height)
{
	int result;
	char id[32];
	ColorModel cmodel;

	if (model<1 || model>NUM_MODELS)
		return -1;

	cc_setBounds(type);

	if (g_rawFrame.m_pixels==NULL)
	{
		cprintf("No raw frame in memory!\n");
		return -2;
	}

	// create lut
	result = g_blobs->generateLUT(model, g_rawFrame, RectA(xoffset, yoffset, width, height), &cmodel);
	if (result<0)
	{
		cprintf("Color saturation isn't high enough!\n");
		return result;
	}

	cmodel.m_type = type;

	// save to flash
	sprintf(id, "signature%d", model);
	prm_set(id, INTS8(sizeof(ColorModel), &cmodel), END);
	prm_setDirty(false); // prevent reload (because we don't want to load the lut (yet) and lose our frame

	cprintf("Success!\n");

	return result;
}

int32_t cc_setSigPoint(const uint32_t &type, const uint8_t &model, const uint16_t &x, const uint16_t &y, Chirp *chirp)
{
	RectA region;
	int result; 
	char id[32];
	ColorModel cmodel;

	if (model<1 || model>NUM_MODELS)
		return -1;

	cc_setBounds(type);

	if (g_rawFrame.m_pixels==NULL)
	{
		cprintf("No raw frame in memory!\n");
		return -2;
	}

	result = g_blobs->generateLUT(model, g_rawFrame, Point16(x, y), &cmodel, &region);
  	if (result<0)
	{
		cprintf("Color saturation isn't high enough!\n");
		return result;
	}

	if (chirp)
	{
		BlobA blob(model, region.m_xOffset, region.m_xOffset+region.m_width, region.m_yOffset, region.m_yOffset+region.m_height);
		cc_sendBlobs(chirp, &blob, 1, RENDER_FLAG_FLUSH | RENDER_FLAG_BLEND_BG);
	}

	cmodel.m_type = type;

	// save to flash
	sprintf(id, "signature%d", model);
	prm_set(id, INTS8(sizeof(ColorModel), &cmodel), END);
	prm_setDirty(false); // prevent reload (because we don't want to load the lut (yet) and lose our frame

	cprintf("Success!\n");

	return result;
}

int32_t cc_clearSig(const uint8_t &model)
{
	char id[32];
	ColorModel cmodel;
	int res;

 	if (model<1 || model>NUM_MODELS)
		return -1;

	memset(&cmodel, 0, sizeof(cmodel));

	sprintf(id, "signature%d", model);
	res = prm_set(id, INTS8(sizeof(ColorModel), &cmodel), END);

	// update lut
 	cc_loadLut();

	return res;
}

int32_t cc_clearAllSig()
{
	char id[32];
	uint8_t model;
	ColorModel cmodel;
	int res; 

	memset(&cmodel, 0, sizeof(cmodel));

   	for (model=1; model<=NUM_MODELS; model++)
	{
		sprintf(id, "signature%d", model);
		res = prm_set(id, INTS8(sizeof(ColorModel), &cmodel), END);
		if (res<0)
			return res;			
	}

	// update lut
 	cc_loadLut();

	return 0;
}


int32_t cc_getRLSFrameChirp(Chirp *chirp)
{
	return cc_getRLSFrameChirpFlags(chirp);
}

int32_t cc_getRLSFrameChirpFlags(Chirp *chirp, uint8_t renderFlags)
{
	int32_t result;
	uint32_t len, numRls;

	if (g_rawFrame.m_pixels)
		cc_loadLut();

	g_qqueue->flush();

	// figure out prebuf length (we need the prebuf length and the number of runlength segments, but there's a chicken and egg problem...)
	len = Chirp::serialize(chirp, RLS_MEMORY, RLS_MEMORY_SIZE,  HTYPE(0), UINT16(0), UINT16(0), UINTS32_NO_COPY(0), END);

	result = cc_getRLSFrame((uint32_t *)(RLS_MEMORY+len), LUT_MEMORY);
	// copy from IPC memory to RLS_MEMORY
	numRls = g_qqueue->readAll((Qval *)(RLS_MEMORY+len), (RLS_MEMORY_SIZE-len)/sizeof(Qval));
	Chirp::serialize(chirp, RLS_MEMORY, RLS_MEMORY_SIZE,  HTYPE(FOURCC('C','C','Q','1')), HINT8(renderFlags), UINT16(CAM_RES2_WIDTH), UINT16(CAM_RES2_HEIGHT), UINTS32_NO_COPY(numRls), END);
	// send frame, use in-place buffer
	chirp->useBuffer(RLS_MEMORY, len+numRls*4);

	return result;
}

int32_t cc_getRLSFrame(uint32_t *memory, uint8_t *lut, bool sync)
{
	int32_t res;
	int32_t responseInt = -1;

	// check mode, set if necessary
	if ((res=cam_setMode(CAM_MODE1))<0)
		return res;

	// forward call to M0, get frame
	if (sync)
	{
		g_chirpM0->callSync(g_getRLSFrameM0, 
			UINT32((uint32_t)memory), UINT32((uint32_t)lut), END_OUT_ARGS,
			&responseInt, END_IN_ARGS);
		return responseInt;
	}
	else
	{
		g_chirpM0->callAsync(g_getRLSFrameM0, 
			UINT32((uint32_t)memory), UINT32((uint32_t)lut), END_OUT_ARGS);
		return 0;
	}

}

int32_t cc_setMemory(const uint32_t &location, const uint32_t &len, const uint8_t *data)
{
	uint32_t i;
	uint8_t *dest = (uint8_t *)location;
	for (i=0; i<len; i++)
		dest[i] = data[i];

	return len;
}

int cc_sendBlobs(Chirp *chirp, const BlobA *blobs, uint32_t len, uint8_t renderFlags)
{
	CRP_RETURN(chirp, HTYPE(FOURCC('C','C','B','1')), HINT8(renderFlags), HINT16(CAM_RES2_WIDTH), HINT16(CAM_RES2_HEIGHT), UINTS16(len*sizeof(BlobA)/sizeof(uint16_t), blobs), END);
	return 0;
}

int cc_sendBlobs(Chirp *chirp, const BlobA *blobs, uint32_t len, const BlobB *ccBlobs, uint32_t ccLen, uint8_t renderFlags)
{
	CRP_RETURN(chirp, HTYPE(FOURCC('C','C','B','2')), HINT8(renderFlags), HINT16(CAM_RES2_WIDTH), HINT16(CAM_RES2_HEIGHT), UINTS16(len*sizeof(BlobA)/sizeof(uint16_t), blobs), UINTS16(ccLen*sizeof(BlobB)/sizeof(uint16_t), ccBlobs), END);
	return 0;
}

uint8_t ledBrightness(uint32_t area)
{
	uint32_t brightness;

	brightness = 0x100*area/20000;
	if (brightness==0) // can't take log of 0...
		return 1;
	
	// put on log curve
	brightness = log((float)brightness)*50;
	// saturate
	if (brightness>0xff)
		brightness = 0xff;
	else if (brightness==0) 
		brightness = 1;

	return brightness;
}

void cc_setLED()
{
	BlobA *blob;
	uint32_t area, color, r, g, b;
	uint8_t brightness;

	blob = (BlobA *)g_blobs->getMaxBlob();
	if (blob)
	{
		if (blob->m_model<=NUM_MODELS)
			color = g_colors[blob->m_model];
		else
			color = g_colors[0];

		area = (blob->m_right - blob->m_left)*(blob->m_bottom - blob->m_top);
		brightness = ledBrightness(area);
		b = color&0xff;
		b = b ? (b*brightness>>8)+1 : 0;
		color >>= 8;
		g = color&0xff;
		g = g ? (g*brightness>>8)+1 : 0;
		color >>= 8;
		r = color&0xff;
		r = r ? (r*brightness>>8)+1 : 0;
		led_setRGB(r, g, b);
	}
	else
		led_set(0);
}


