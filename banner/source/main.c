/*******************************************************************************
 * main.c
 *
 * Copyright (c) 2009 The Lemon Man
 * Copyright (c) 2009 Nicksasa
 * Copyright (c) 2009 WiiPower
 *
 * Distributed under the terms of the GNU General Public License (v2)
 * See http://www.gnu.org/licenses/gpl-2.0.txt for more info.
 *
 * Description:
 * -----------
 *
 ******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <stdlib.h>
#include <gccore.h>
#include <fat.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include <ogc/gx.h>

#include "main.h"
#include "tools.h"
#include "lz77.h"
#include "u8.h"
#include "config.h"
#include "patch.h"
#include "codes/codes.h"
#include "codes/patchcode.h"
#include "nand.h"
#include "background.h"

#define Vector guVector
#define DEFAULT_FIFO_SIZE	(256*1024)

//void *xfb[2] = { NULL, NULL};
u32 *xfb = NULL;
GXRModeObj *rmode = NULL;
u8 Video_Mode;
Mtx GXmodelView2D;
int whichfb = 0;
void *gp_fifo = NULL;

void*	dolchunkoffset[64];			//TODO: variable size
u32		dolchunksize[64];			//TODO: variable size
u32		dolchunkcount;

void _unstub_start();

// Prevent IOS36 loading at startup
s32 __IOS_LoadStartupIOS()
{
	return 0;
}

static void power_cb() 
{
	Power_Flag = true;
}

static void reset_cb() 
{
	Reset_Flag = true;
}

typedef void (*entrypoint) (void);

typedef struct _dolheader
{
	u32 text_pos[7];
	u32 data_pos[11];
	u32 text_start[7];
	u32 data_start[11];
	u32 text_size[7];
	u32 data_size[11];
	u32 bss_start;
	u32 bss_size;
	u32 entry_point;
} dolheader;


typedef struct _dirent
{
	char name[ISFS_MAXPATH + 1];
	int type;
} dirent_t;

bool file_exists(const char * filename)
{
	FILE *ftest = fopen(filename, "r");
	if (ftest)
	{
		fclose(ftest);
		return true;
	}
	return false;
}

s32 __FileCmp(const void *a, const void *b)
{
	dirent_t *hdr1 = (dirent_t *)a;
	dirent_t *hdr2 = (dirent_t *)b;
	
	if (hdr1->type == hdr2->type)
	{
		return strcmp(hdr1->name, hdr2->name);
	} else
	{
		return 0;
	}
}



s32 getdir(char *path, dirent_t **ent, u32 *cnt)
{
	s32 res;
	u32 num = 0;

	int i, j, k;
	
	res = ISFS_ReadDir(path, NULL, &num);
	if(res != ISFS_OK)
	{
		printf("Error: could not get dir entry count! (result: %d)\n", res);
		return -1;
	}

	char *nbuf = (char *)allocate_memory((ISFS_MAXPATH + 1) * num);
	char ebuf[ISFS_MAXPATH + 1];

	if(nbuf == NULL)
	{
		printf("Error: could not allocate buffer for name list!\n");
		return -2;
	}

	res = ISFS_ReadDir(path, nbuf, &num);
	if(res != ISFS_OK)
	{
		printf("Error: could not get name list! (result: %d)\n", res);
		return -3;
	}
	
	*cnt = num;
	
	*ent = allocate_memory(sizeof(dirent_t) * num);

	for(i = 0, k = 0; i < num; i++)
	{	    
		for(j = 0; nbuf[k] != 0; j++, k++)
			ebuf[j] = nbuf[k];
		ebuf[j] = 0;
		k++;

		strcpy((*ent)[i].name, ebuf);
	}
	
	qsort(*ent, *cnt, sizeof(dirent_t), __FileCmp);
	
	free(nbuf);
	return 0;
}


char *get_banner_app_name(u64 titleid)
{
	s32 cfd;
    s32 ret;
	u32 num;
	dirent_t *list;
    char contentpath[ISFS_MAXPATH];
    char path[ISFS_MAXPATH];

    u32 cnt = 0;
	char *out;
	u8 *buffer = allocate_memory(800);
	   
	sprintf(contentpath, "/title/%08x/%08x/content", TITLE_UPPER(titleid), TITLE_LOWER(titleid));
    ret = getdir(contentpath, &list, &num);
    if (ret < 0)
	{
		printf("Reading folder of the title failed\n");
		free(buffer);
		return NULL;
	}
	
	u8 imet[4] = {0x49, 0x4D, 0x45, 0x54};
	for(cnt=0; cnt < num; cnt++)
    {        
        if(strstr(list[cnt].name, ".app") != NULL || strstr(list[cnt].name, ".APP") != NULL) 
        {
			memset(buffer, 0x00, 800);
            sprintf(path, "/title/%08x/%08x/content/%s", TITLE_UPPER(titleid), TITLE_LOWER(titleid), list[cnt].name);
  
            cfd = ISFS_Open(path, ISFS_OPEN_READ);
            if (cfd < 0)
			{
	    	    printf("ISFS_OPEN for %s failed %d\n", path, cfd);
				continue;
			}
			
            ret = ISFS_Read(cfd, buffer, 800);
	        if (ret < 0)
	        {
	    	    printf("ISFS_Read for %s failed %d\n", path, ret);
		        ISFS_Close(cfd);
				continue;
	        }

            ISFS_Close(cfd);	
              
			if(memcmp((buffer+0x80), imet, 4) == 0)
			{
				//printf("FOUND IT!\n");	
				out = list[cnt].name;
				//sleep(3);
				free(buffer);
				free(list);
				
				return out;
			}
			    
        }
    }
	
	free(buffer);
	free(list);
	out = NULL;
	return NULL;

}


void videoInit()
{
	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(0);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();
 	
    int x = 32, y = 212, w, h;
    w = rmode->fbWidth - 64;
    h = rmode->xfbHeight - 212 - 32;

	CON_InitEx(rmode, x, y, w, h);
	
	// Set console text color
	printf("\x1b[%u;%um", 37, false);
	printf("\x1b[%u;%um", 40, false);
	
//    int x = 24, y = 32, w, h;
//    w = rmode->fbWidth - (32);
//    h = rmode->xfbHeight - (48);
//	CON_InitEx(rmode, x, y+240, w, h-240);

	VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
}

void GX_videoInit__()
{
	f32 yscale;
	u32 xfbHeight;
	Mtx perspective;
/*	
	rmode = VIDEO_GetPreferredMode(NULL);

	if (CONF_GetAspectRatio() == CONF_ASPECT_16_9)
	{
		rmode->viWidth = 678;
		rmode->viXOrigin = (VI_MAX_WIDTH_NTSC - 678)/2;
	}

	VIDEO_Configure (rmode);

	//xfb[0] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	//xfb[1] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	//VIDEO_ClearFrameBuffer (rmode, xfb[0], COLOR_BLACK);
	//VIDEO_ClearFrameBuffer (rmode, xfb[1], COLOR_BLACK);

	//VIDEO_SetNextFramebuffer(xfb[whichfb]);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();
*/
	gp_fifo = memalign(32,DEFAULT_FIFO_SIZE);
	memset(gp_fifo,0,DEFAULT_FIFO_SIZE);

	GX_Init (gp_fifo, DEFAULT_FIFO_SIZE);

	GXColor background = { 21, 35, 40, 0xff };
	GX_SetCopyClear (background, 0x00ffffff);

	yscale = GX_GetYScaleFactor(rmode->efbHeight,rmode->xfbHeight);
	xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0,0,rmode->fbWidth,rmode->efbHeight);
	GX_SetDispCopySrc(0,0,rmode->fbWidth,rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth,xfbHeight);
	GX_SetCopyFilter(rmode->aa,rmode->sample_pattern,GX_TRUE,rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,((rmode->viHeight==2*rmode->xfbHeight)?GX_ENABLE:GX_DISABLE));

	if (rmode->aa){
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	}
	else{
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	}

	GX_SetDispCopyGamma(GX_GM_1_0);

	GX_ClearVtxDesc();
	GX_InvVtxCache ();
	GX_InvalidateTexAll();

	GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc (GX_VA_CLR0, GX_DIRECT);

	GX_SetVtxAttrFmt (GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt (GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetZMode (GX_FALSE, GX_LEQUAL, GX_TRUE);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTevOp (GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

	guMtxIdentity(GXmodelView2D);
	guMtxTransApply (GXmodelView2D, GXmodelView2D, 0.0F, 0.0F, -50.0F);
	GX_LoadPosMtxImm(GXmodelView2D,GX_PNMTX0);

	guOrtho(perspective,0,479,0,639,0,300);
	GX_LoadProjectionMtx(perspective, GX_ORTHOGRAPHIC);

	GX_SetViewport(0,0,rmode->fbWidth,rmode->efbHeight,0,1);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetAlphaUpdate(GX_TRUE);
	
	GX_SetCullMode(GX_CULL_NONE);
}

s32 get_tpl_vc(GXTexObj *TexObj, unsigned short *heighttemp, unsigned short *widthtemp, u64 titleid)
{
	u32 size_out = 0;
    char path[ISFS_MAXPATH];
    char tplpath[ISFS_MAXPATH];
    char *bannerapp;

	u32 banner_size;
	u32 tpl_size = 0;
	u32 decompressed_banner_size;
	//u32 titlehex = TITLE_LOWER(titleid);

	//printf("GAME: %s\n", get_name(titleid));
	
	/*
		4a snes - 07
		46 nes - 00
		50 tgfx - 09
		4d genesis - 00
	*/
	
   	s32 ret;

	//printf("TPL Target: %s\n", tplpath);
	//printf("loading tpl data\n");

	bannerapp = get_banner_app_name(titleid);
	if(bannerapp == NULL)
	{
		printf("Banner not found !\n");
		TexObj = NULL;
		return -1;
	}	
	//printf("BANNER APP: %s\n",bannerapp);

	u8 *banner;
	u8 *decompressed_banner;
	u8 *tpl;
	u8 *compressed;
	
	sprintf(path, "/title/%08x/%08x/content/%s", TITLE_UPPER(titleid), TITLE_LOWER(titleid), bannerapp);
	printf("PATH: %s\n", path);
	
	ret = read_file(path, &compressed, &size_out);
	if (ret < 0)
	{
		printf("Reading file failed\n");
		return ret;
	}
	//do_U8_archive(compressed+0x640, u8path);
	ret = do_file_U8_archive(compressed+0x640, "banner.bin", &banner, &banner_size);
	if (ret < 0)
	{
		printf("Reading banner failed\n");
		free(compressed);
		return ret;
	}
	
	/*	switch (*(char *)&titlehex)
	{
		case 'W':
		sprintf(u8path, "sd:/%08x/meta/banner.bin", TITLE_LOWER(titleid));
		break;
			
		default:
		sprintf(u8path, "sd:/%08x/meta/banner.bin", TITLE_LOWER(titleid));
		break;
	}*/
	//sprintf(u8path, "sd:/%08x/meta/banner.bin", TITLE_LOWER(titleid));
	//banner_size = read_sd(u8path, &banner);
	ret = decompressLZ77content(banner+0x24, banner_size, &decompressed_banner, &decompressed_banner_size);
	if (ret < 0)
	{
		free(compressed);
		free(banner);
		return ret;
	}
	//sprintf(u8path, "sd:/%08x/extracted", TITLE_LOWER(titleid));
	//do_U8_archive(decompressed_banner, u8path);

	//printf("TPL: %s\n", tplpath);

	sprintf(tplpath, "VCPic.tpl");
	ret = do_file_U8_archive(decompressed_banner, tplpath, &tpl, &tpl_size);
	if (ret < 0)
	{
		sprintf(tplpath, "banner_logo_n.tpl");
		ret = do_file_U8_archive(decompressed_banner, tplpath, &tpl, &tpl_size);
		if (ret < 0)
		{
			sprintf(tplpath, "BannerImage.tpl");
			ret = do_file_U8_archive(decompressed_banner, tplpath, &tpl, &tpl_size);
			if (ret < 0)
			{
				printf("Reading tpl failed\n");
				free(compressed);
				free(banner);
				return ret;
			}
		}
	}
		
	//sleep(1);

	memcpy((void*)&heighttemp,tpl + 0x14, 2);
	memcpy((void*)&widthtemp,tpl + 0x14 + 2, 2);

	//tpl_size = read_sd(tplpath, &tpl);		
	TPLFile tplfile;
        
	ret = TPL_OpenTPLFromMemory(&tplfile, tpl, tpl_size);
	if(ret < 0) 
	{
		free(tpl);
		free(banner);
		free(compressed);
		free(decompressed_banner);
		return -1;
	}

	ret = TPL_GetTexture(&tplfile,0,TexObj);
	if(ret < 0) 
	{
		free(tpl);
		free(compressed);
		free(decompressed_banner);
		return -2;
	}
	TPL_CloseTPLFile(&tplfile);
	
	free(tpl);
	free(banner);
	free(compressed);
	free(decompressed_banner);

	printf("DONE!\n");
	
	//return tpl_size;		
	return 0;
}		

void gfx_draw_image(f32 xpos, f32 ypos, u16 width, u16 height, GXTexObj texObj, float degrees, float scaleX, f32 scaleY, u8 alpha )
//---------------------------------------------------------------------------------
{	

		GX_LoadTexObj(&texObj, GX_TEXMAP0);

		GX_SetTevOp (GX_TEVSTAGE0, GX_MODULATE);
		GX_SetVtxDesc (GX_VA_TEX0, GX_DIRECT);

		Mtx m,m1,m2, mv;
		width *=.5;
		height*=.5;
		guMtxIdentity (m1);
		guMtxScaleApply(m1,m1,scaleX,scaleY,1.0);
		Vector axis =(Vector) {0 , 0, 1 };
		guMtxRotAxisDeg (m2, &axis, degrees);
		guMtxConcat(m2,m1,m);

		guMtxTransApply(m,m, xpos+width,ypos+height,0);
		guMtxConcat (GXmodelView2D, m, mv);
		GX_LoadPosMtxImm (mv, GX_PNMTX0);
	
		GX_Begin(GX_QUADS, GX_VTXFMT0,4);
		GX_Position3f32(-width, -height,  0);
		GX_Color4u8(0xFF,0xFF,0xFF,alpha);
		GX_TexCoord2f32(0, 0);
  
		GX_Position3f32(width, -height,  0);
		GX_Color4u8(0xFF,0xFF,0xFF,alpha);
		GX_TexCoord2f32(1, 0);
  
		GX_Position3f32(width, height,  0);
		GX_Color4u8(0xFF,0xFF,0xFF,alpha);
		GX_TexCoord2f32(1, 1);
	
		GX_Position3f32(-width, height,  0);
		GX_Color4u8(0xFF,0xFF,0xFF,alpha);
		GX_TexCoord2f32(0, 1);
		GX_End();
		GX_LoadPosMtxImm (GXmodelView2D, GX_PNMTX0);

		GX_SetTevOp (GX_TEVSTAGE0, GX_PASSCLR);
		GX_SetVtxDesc (GX_VA_TEX0, GX_NONE);
		
}

void gfx_render_direct()
//---------------------------------------------------------------------------------
{
    GX_DrawDone ();
	whichfb ^= 1;		// flip framebuffer
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	//GX_CopyDisp(xfb[whichfb],GX_TRUE);
	GX_CopyDisp(xfb,GX_TRUE);
	//VIDEO_SetNextFramebuffer(xfb[whichfb]);
	VIDEO_SetNextFramebuffer(xfb);
 	VIDEO_Flush();
 	VIDEO_WaitVSync();
}

s32 get_game_list(char ***TitleIds, u32 *num)
{
	int ret;
	u32 maxnum;
	u32 tempnum = 0;
	u32 number;
	dirent_t *list = NULL;
	dirent_t *templist;
    char path[ISFS_MAXPATH];
    sprintf(path, "/title/00010001");
    ret = getdir(path, &list, &maxnum);
    if (ret < 0)
	{
		printf("Reading folder /title/00010001 failed\n");
		return ret;
	}

	char **temp = allocate_memory(maxnum*4);
	if (temp == NULL)
	{
		free(list);
		printf("Out of memory\n");
		return -1;
	}

	int i;
	for (i = 0; i < maxnum; i++)
	{	
		if (memcmp(list[i].name, "48", 2) != 0 && memcmp(list[i].name, "55", 2) != 0) // Ignore channels starting with H (Channels) and U (Loadstructor channels)
		{
			sprintf(path, "/title/00010001/%s/content", list[i].name);
			
			// Dirty workaround, ISFS_ReadDir does not work properly on nand emu
			templist = NULL;
			ret = getdir(path, &templist, &number);	
			if (ret >= 0 && templist != NULL)
			{
				free(templist);
			}
			
			if (ret >= 0 && number > 1) // 1 == tmd only
			{
				temp[tempnum] = allocate_memory(10);
				memset(temp[tempnum], 0x00, 10);
				memcpy(temp[tempnum], list[i].name, 8);	
				tempnum++;		
			}
		}
	}

	*TitleIds = temp;
	*num = tempnum;
	free(list);
	return 0;
}


s32 check_dol(u64 titleid, char *out, u16 bootcontent)
{
	s32 cfd;
    s32 ret;
	u32 num;
	dirent_t *list;
    char contentpath[ISFS_MAXPATH];
    char path[ISFS_MAXPATH];
    int cnt = 0;
	
	u8 LZ77_0x10 = 0x10;
    u8 LZ77_0x11 = 0x11;
	u8 *decompressed;
	u8 *compressed;
	u32 size_out = 0;
	u32 decomp_size = 0;
	
	

    u8 *buffer = allocate_memory(8);
	if (buffer == NULL)
	{
		printf("Out of memory\n");
		return -1;
	}
	
    u8 check[6] = {0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
 
    sprintf(contentpath, "/title/%08x/%08x/content", TITLE_UPPER(titleid), TITLE_LOWER(titleid));
    ret = getdir(contentpath, &list, &num);
    if (ret < 0)
	{
		printf("Reading folder of the title failed\n");
		free(buffer);
		return ret;
	}
	for(cnt=0; cnt < num; cnt++)
    {        
        if ((strstr(list[cnt].name, ".app") != NULL || strstr(list[cnt].name, ".APP") != NULL) && (strtol(list[cnt].name, NULL, 16) != bootcontent))
        {			
			memset(buffer, 0x00, 8);
            sprintf(path, "/title/%08x/%08x/content/%s", TITLE_UPPER(titleid), TITLE_LOWER(titleid), list[cnt].name);
  
            cfd = ISFS_Open(path, ISFS_OPEN_READ);
            if (cfd < 0)
			{
	    	    printf("ISFS_Open for %s failed %d\n", path, cfd);
				continue; 
			}

            ret = ISFS_Read(cfd, buffer, 7);
	        if (ret < 0)
	        {
	    	    printf("ISFS_Read for %s failed %d\n", path, ret);
		        ISFS_Close(cfd);
				continue;
	        }

            ISFS_Close(cfd);	

			if (buffer[0] == LZ77_0x10 || buffer[0] == LZ77_0x11)
			{
                if (buffer[0] == LZ77_0x10)
				{
					printf("Found LZ77 0x10 compressed content --> %s\n", list[cnt].name);
				} else
				{
					printf("Found LZ77 0x11 compressed content --> %s\n", list[cnt].name);
				}
				printf("This is most likely the main DOL, decompressing for checking\n");
				ret = read_file(path, &compressed, &size_out);
				if (ret < 0)
				{
					printf("Reading file failed\n");
					free(buffer);
					free(list);
					return ret;
				}
				printf("read file\n");
				ret = decompressLZ77content(compressed, 32, &decompressed, &decomp_size);
				if (ret < 0)
				{
					printf("Decompressing failed\n");
					free(buffer);
					free(list);
					return ret;
				}				
				memcpy(buffer, decompressed, 8);
 			}
			
	        ret = memcmp(buffer, check, 6);
            if(ret == 0)
            {
				printf("Found DOL --> %s\n", list[cnt].name);
				sprintf(out, "%s", path);
				free(buffer);
				free(list);
				return 0;
            } 
        }
    }
	
	free(buffer);
	free(list);
	
	printf("No .dol found\n");
	return -1;
}

void patch_dol(bool bootcontent)
{
	s32 ret;
	int i;
	
	for (i=0;i < dolchunkcount;i++)
	{		
		if (!bootcontent)
		{
			if (languageoption != -1)
			{
				ret = patch_language(dolchunkoffset[i], dolchunksize[i], languageoption);
			}
			
			if (videopatchoption != 0)
			{
				search_video_modes(dolchunkoffset[i], dolchunksize[i]);
				patch_video_modes_to(rmode, videopatchoption);
			}
		}

		if (hooktypeoption != 0)
		{
			// Before this can be done, the codehandler needs to be in memory, and the code to patch needs to be in the right pace
			dochannelhooks(dolchunkoffset[i], dolchunksize[i]);	
		}
	}
}  


u32 load_dol(u8 *buffer)
{
	dolchunkcount = 0;
	
	dolheader *dolfile;
	dolfile = (dolheader *)buffer;
	
	printf("Entrypoint: %08x\n", dolfile->entry_point);
	printf("BSS: %08x, size = %08x(%u)\n", dolfile->bss_start, dolfile->bss_size, dolfile->bss_size);

	memset((void *)dolfile->bss_start, 0, dolfile->bss_size);
	DCFlushRange((void *)dolfile->bss_start, dolfile->bss_size);
	
    printf("BSS cleared\n");
	
	u32 doloffset;
	u32 memoffset;
	u32 restsize;
	u32 size;

	int i;
	for (i = 0; i < 7; i++)
	{	
		if(dolfile->text_pos[i] < sizeof(dolheader))
			continue;
	    
		dolchunkoffset[dolchunkcount] = (void *)dolfile->text_start[i];
		dolchunksize[dolchunkcount] = dolfile->text_size[i];
		dolchunkcount++;
		
		doloffset = (u32)buffer + dolfile->text_pos[i];
		memoffset = dolfile->text_start[i];
		restsize = dolfile->text_size[i];

		printf("Moving text section %u from %08x to %08x-%08x...", i, dolfile->text_pos[i], dolfile->text_start[i], dolfile->text_start[i]+dolfile->text_size[i]);
		fflush(stdout);
			
		while (restsize > 0)
		{
			if (restsize > 2048)
			{
				size = 2048;
			} else
			{
				size = restsize;
			}
			restsize -= size;
			ICInvalidateRange ((void *)memoffset, size);
			memcpy((void *)memoffset, (void *)doloffset, size);
			DCFlushRange((void *)memoffset, size);
			
			doloffset += size;
			memoffset += size;
		}

		printf("done\n");
		fflush(stdout);			
	}

	for(i = 0; i < 11; i++)
	{
		if(dolfile->data_pos[i] < sizeof(dolheader))
			continue;
		
		dolchunkoffset[dolchunkcount] = (void *)dolfile->data_start[i];
		dolchunksize[dolchunkcount] = dolfile->data_size[i];
		dolchunkcount++;

		doloffset = (u32)buffer + dolfile->data_pos[i];
		memoffset = dolfile->data_start[i];
		restsize = dolfile->data_size[i];

		printf("Moving data section %u from %08x to %08x-%08x...", i, dolfile->data_pos[i], dolfile->data_start[i], dolfile->data_start[i]+dolfile->data_size[i]);
		fflush(stdout);
			
		while (restsize > 0)
		{
			if (restsize > 2048)
			{
				size = 2048;
			} else
			{
				size = restsize;
			}
			restsize -= size;
			ICInvalidateRange ((void *)memoffset, size);
			memcpy((void *)memoffset, (void *)doloffset, size);
			DCFlushRange((void *)memoffset, size);
			
			doloffset += size;
			memoffset += size;
		}

		printf("done\n");
		fflush(stdout);			
	} 
	return dolfile->entry_point;
}


s32 search_and_read_dol(u64 titleid, u8 **contentBuf, u32 *contentSize, bool skip_bootcontent)
{
	char filepath[ISFS_MAXPATH] ATTRIBUTE_ALIGN(0x20);
	int ret;
	u16 bootindex;
	u16 bootcontent;
	bool bootcontent_loaded;
	
	u8 *tmdBuffer = NULL;
	u32 tmdSize;
	tmd_content *p_cr;

	u32 pressed;
	u32 pressedGC;

	printf("Reading TMD...");

	sprintf(filepath, "/title/%08x/%08x/content/title.tmd", TITLE_UPPER(titleid), TITLE_LOWER(titleid));
	ret = read_file(filepath, &tmdBuffer, &tmdSize);
	if (ret < 0)
	{
		printf("Reading TMD failed\n");
		return ret;
	}
	printf("done\n");
	
	bootindex = ((tmd *)SIGNATURE_PAYLOAD((signed_blob *)tmdBuffer))->boot_index;
	p_cr = TMD_CONTENTS(((tmd *)SIGNATURE_PAYLOAD((signed_blob *)tmdBuffer)));
	bootcontent = p_cr[bootindex].cid;

	free(tmdBuffer);

	// Write bootcontent to filepath and overwrite it in case another .dol is found
	sprintf(filepath, "/title/%08x/%08x/content/%08x.app", TITLE_UPPER(titleid), TITLE_LOWER(titleid), bootcontent);

	if (skip_bootcontent)
	{
		bootcontent_loaded = false;
		printf("Searching for main DOL...\n");
			
		ret = check_dol(titleid, filepath, bootcontent);
		if (ret < 0)
		{
			printf("Searching for main.dol failed\n");
			printf("Press A to load nand loader instead...\n");
			waitforbuttonpress(&pressed, &pressedGC);
			if (pressed != WPAD_BUTTON_A && pressedGC != PAD_BUTTON_A)
			{
				printf("Other button pressed\n");
				return ret;
			}
			bootcontent_loaded = true;
		}
	} else
	{
		bootcontent_loaded = true;
	}
	
    printf("Loading DOL: %s\n", filepath);
	
	ret = read_file(filepath, contentBuf, contentSize);
	if (ret < 0)
	{
		printf("Reading .dol failed\n");
		return ret;
	}
	
	if (isLZ77compressed(*contentBuf))
	{
		u8 *decompressed;
		ret = decompressLZ77content(*contentBuf, *contentSize, &decompressed, contentSize);
		if (ret < 0)
		{
			printf("Decompression failed\n");
			free(*contentBuf);
			return ret;
		}
		free(*contentBuf);
		*contentBuf = decompressed;
	}	
	
	if (bootcontent_loaded)
	{
		return 1;
	} else
	{
		return 0;
	}
}


void determineVideoMode(u64 titleid)
{
	if (videooption == 0)
	{
		char Region = (char)((u32)titleid % 256);
		
		// Get rmode and Video_Mode for system settings first
		u32 tvmode = CONF_GetVideo();

		// Attention: This returns &TVNtsc480Prog for all progressive video modes
		rmode = VIDEO_GetPreferredMode(0);
		
		switch (tvmode) 
		{
			case CONF_VIDEO_PAL:
				if (CONF_GetEuRGB60() > 0) 
				{
					Video_Mode = VI_EURGB60;
				}
				else 
				{
					Video_Mode = VI_PAL;
				}
				break;

			case CONF_VIDEO_MPAL:
				Video_Mode = VI_MPAL;
				break;

			case CONF_VIDEO_NTSC:
			default:
				Video_Mode = VI_NTSC;
				
		}

		// Overwrite rmode and Video_Mode when Default Video Mode is selected and Wii region doesn't match the channel region
		u32 low;
		low = TITLE_LOWER(titleid);
		if (*(char *)&low != 'W') // Don't overwrite video mode for WiiWare
		{
			switch (Region & 0xFF) 
			{
				case 'P':
				case 'D':
				case 'F':
				case 'X':
				case 'Y':
					if (CONF_GetVideo() != CONF_VIDEO_PAL)
					{
						Video_Mode = VI_EURGB60;

						if (CONF_GetProgressiveScan() > 0 && VIDEO_HaveComponentCable())
						{
							rmode = &TVNtsc480Prog; // This seems to be correct!
						}
						else
						{
							rmode = &TVEurgb60Hz480IntDf;
						}				
					}
					break;

				case 'E':
				case 'J':
				default:
					if (CONF_GetVideo() != CONF_VIDEO_NTSC)
					{
						Video_Mode = VI_NTSC;
						if (CONF_GetProgressiveScan() > 0 && VIDEO_HaveComponentCable())
						{
							rmode = &TVNtsc480Prog;
						}
						else
						{
							rmode = &TVNtsc480IntDf;
						}				
					}
			}
		}
	} else
	{
		if (videooption == 1)
		{
			rmode = &TVNtsc480IntDf;
		} else
		if (videooption == 2)
		{
			rmode = &TVNtsc480Prog;
		} else
		if (videooption == 3)
		{
			rmode = &TVEurgb60Hz480IntDf;
		} else
		if (videooption == 4)
		{
			rmode = &TVEurgb60Hz480Prog;
		} else
		if (videooption == 5)
		{
			rmode = &TVPal528IntDf;
		} else
		if (videooption == 6)
		{
			rmode = &TVMpal480IntDf;
		} else
		if (videooption == 7)
		{
			rmode = &TVMpal480Prog;
		}
		Video_Mode = (rmode->viTVMode) >> 2;
	}
}

void setVideoMode()
{	
	*(u32 *)0x800000CC = Video_Mode;
	DCFlushRange((void*)0x800000CC, sizeof(u32));
	
	// Overwrite all progressive video modes as they are broken in libogc
	if (videomode_interlaced(rmode) == 0)
	{
		rmode = &TVNtsc480Prog;
	}

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	
	if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

bool check_text(char *s) 
{
    int i = 0;
    for(i=0; i < strlen(s); i++)
    {
        if (s[i] < 32 || s[i] > 165)
		{
			return false;
		}
	}  

	return true;
}

char *read_name_from_banner_app(u64 titleid)
{
	s32 cfd;
    s32 ret;
	u32 num;
	dirent_t *list;
    char contentpath[ISFS_MAXPATH];
    char path[ISFS_MAXPATH];
	int i;
    int length;
    u32 cnt = 0;
	char *out;
	u8 *buffer = allocate_memory(800);
	   
	sprintf(contentpath, "/title/%08x/%08x/content", TITLE_UPPER(titleid), TITLE_LOWER(titleid));
	
    ret = getdir(contentpath, &list, &num);
    if (ret < 0)
	{
		printf("Reading folder of the title failed\n");
		free(buffer);
		return NULL;
	}
	
	u8 imet[4] = {0x49, 0x4D, 0x45, 0x54};
	for(cnt=0; cnt < num; cnt++)
    {        
        if (strstr(list[cnt].name, ".app") != NULL || strstr(list[cnt].name, ".APP") != NULL) 
        {
			memset(buffer, 0x00, 800);
            sprintf(path, "/title/%08x/%08x/content/%s", TITLE_UPPER(titleid), TITLE_LOWER(titleid), list[cnt].name);
  
            cfd = ISFS_Open(path, ISFS_OPEN_READ);
            if (cfd < 0)
			{
	    	    printf("ISFS_OPEN for %s failed %d\n", path, cfd);
				continue;
			}
			
            ret = ISFS_Read(cfd, buffer, 800);
	        if (ret < 0)
	        {
	    	    printf("ISFS_Read for %s failed %d\n", path, ret);
		        ISFS_Close(cfd);
				continue;
	        }

            ISFS_Close(cfd);	
              
			if(memcmp((buffer+0x80), imet, 4) == 0)
			{
				length = 0;
				i = 0;
				while(buffer[0xF1 + i*2] != 0x00)
				{
					length++;
					i++;
				}
				
				out = allocate_memory(length+10);
				if(out == NULL)
				{
					printf("Allocating memory for buffer failed\n");
					free(buffer);
					return NULL;
				}
				memset(out, 0x00, length+10);
				
				i = 0;
				while(buffer[0xF1 + i*2] != 0x00)
				{
					out[i] = (char) buffer[0xF1 + i*2];
					i++;
				}				
				
				free(buffer);
				free(list);
				
				return out;
			}
			    
        }
    }
	
	free(buffer);
	free(list);
	
	return NULL;
}

char *read_name_from_banner_bin(u64 titleid)
{
	s32 cfd;
    s32 ret;
    char path[ISFS_MAXPATH];
	int i;
    int length;
	char *out;
	u8 *buffer = allocate_memory(160);
   
	// Try to read from banner.bin first
	sprintf(path, "/title/%08x/%08x/data/banner.bin", TITLE_UPPER(titleid), TITLE_LOWER(titleid));
  
	cfd = ISFS_Open(path, ISFS_OPEN_READ);
	if (cfd < 0)
	{
		//printf("ISFS_OPEN for %s failed %d\n", path, cfd);
		return NULL;
	} else
	{
	    ret = ISFS_Read(cfd, buffer, 160);
	    if (ret < 0)
	    {
			printf("ISFS_Read for %s failed %d\n", path, ret);
		    ISFS_Close(cfd);
			free(buffer);
			return NULL;
		}

		ISFS_Close(cfd);	

		length = 0;
		i = 0;
		while(buffer[0x21 + i*2] != 0x00)
		{
			length++;
			i++;
		}
		out = allocate_memory(length+10);
		if(out == NULL)
		{
			printf("Allocating memory for buffer failed\n");
			free(buffer);
			return NULL;
		}
		memset(out, 0x00, length+10);
		
		i = 0;
		while (buffer[0x21 + i*2] != 0x00)
		{
			out[i] = (char) buffer[0x21 + i*2];
			i++;
		}
		
		free(buffer);

		return out;		
	}
 	
	free(buffer);
	
	return NULL;
}

char *get_name(u64 titleid)
{
	char *temp;
	u32 low;
	low = TITLE_LOWER(titleid);

	temp = read_name_from_banner_bin(titleid);
	if (temp == NULL || !check_text(temp))
	{
		temp = read_name_from_banner_app(titleid);
	}
	
	if (temp != NULL)
	{
		if (*(char *)&low == 'W')
		{
			return temp;
		}
		switch(low & 0xFF)
		{
			case 'E':
				memcpy(temp+strlen(temp), " (NTSC-U)", 9);
				break;
			case 'P':
				memcpy(temp+strlen(temp), " (PAL)", 6);
				break;
			case 'J':
				memcpy(temp+strlen(temp), " (NTSC-J)", 9);
				break;	
			case 'L':
				memcpy(temp+strlen(temp), " (PAL)", 6);
				break;	
			case 'N':
				memcpy(temp+strlen(temp), " (NTSC-U)", 9);
				break;		
			case 'M':
				memcpy(temp+strlen(temp), " (PAL)", 6);
				break;
			case 'K':
				memcpy(temp+strlen(temp), " (NTSC)", 7);
				break;
			default:
				break;
				
		}
	}
	
	if (temp == NULL)
	{
		temp = malloc(6);
		memset(temp, 0, 6);
		memcpy(temp, (char *)(&low), 4);
	}
	
	return temp;
}


void bootTitle(u64 titleid)
{
	entrypoint appJump;
	int ret;
	u32 requested_ios;
	u8 *dolbuffer;
	u32 dolsize;
	bool bootcontentloaded;
	
	ret = search_and_read_dol(titleid, &dolbuffer, &dolsize, true);
	if (ret < 0)
	{
		printf(".dol loading failed\n");
		return;
	}
	bootcontentloaded = (ret == 1);

	determineVideoMode(titleid);
	
	entryPoint = load_dol(dolbuffer);
	
	free(dolbuffer);

	printf(".dol loaded\n");

	ret = identify(titleid, &requested_ios);
	if (ret < 0)
	{
		printf("Identify failed\n");
		return;
	}
	
	ISFS_Deinitialize();
	
	// Set the clock
	settime(secs_to_ticks(time(NULL) - 946684800));

	if (entryPoint != 0x3400)
	{
		printf("Setting bus speed\n");
		*(u32*)0x800000F8 = 0x0E7BE2C0;
		printf("Setting cpu speed\n");
		*(u32*)0x800000FC = 0x2B73A840;

		DCFlushRange((void*)0x800000F8, 0xFF);
	}
	
	// Remove 002 error
	printf("Fake IOS Version(%u)\n", requested_ios);
	*(u16 *)0x80003140 = requested_ios;
	*(u16 *)0x80003142 = 0xffff;
	*(u16 *)0x80003188 = requested_ios;
	*(u16 *)0x8000318A = 0xffff;
	
	DCFlushRange((void*)0x80003140, 4);
	DCFlushRange((void*)0x80003188, 4);
	
	ret = ES_SetUID(titleid);
	if (ret < 0)
	{
		printf("ES_SetUID failed %d", ret);
		return;
	}	
	printf("ES_SetUID successful\n");
	
	
	if (hooktypeoption != 0)
	{
		do_codes(titleid);
	}
	
	patch_dol(bootcontentloaded);

	printf("Loading complete, booting...\n");

	appJump = (entrypoint)entryPoint;

	//sleep(5);

	setVideoMode();
	
	WPAD_Shutdown();
	SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);

	if (entryPoint != 0x3400)
	{
		if (hooktypeoption != 0)
		{
			__asm__(
						"lis %r3, entryPoint@h\n"
						"ori %r3, %r3, entryPoint@l\n"
						"lwz %r3, 0(%r3)\n"
						"mtlr %r3\n"
						"lis %r3, 0x8000\n"
						"ori %r3, %r3, 0x18A8\n"
						"mtctr %r3\n"
						"bctr\n"
						);
						
		} else
		{
			appJump();	
		}
	} else
	{
		if (hooktypeoption != 0)
		{
			__asm__(
						"lis %r3, returnpoint@h\n"
						"ori %r3, %r3, returnpoint@l\n"
						"mtlr %r3\n"
						"lis %r3, 0x8000\n"
						"ori %r3, %r3, 0x18A8\n"
						"mtctr %r3\n"
						"bctr\n"
						"returnpoint:\n"
						"bl DCDisable\n"
						"bl ICDisable\n"
						"li %r3, 0\n"
						"mtsrr1 %r3\n"
						"lis %r4, entryPoint@h\n"
						"ori %r4,%r4,entryPoint@l\n"
						"lwz %r4, 0(%r4)\n"
						"mtsrr0 %r4\n"
						"rfi\n"
						);
		} else
		{
			_unstub_start();
		}
	}
}

#define menuitems 8

void show_menu()
{
	int i;
	u32 pressed;
	u32 pressedGC;
	int ret;
	GXTexObj TexObj;
	unsigned short heighttemp = 0;
	unsigned short widthtemp = 0;
	s32 lastbanner = -1;

	int selection = 0;
	u32 optioncount[menuitems] = { 1, 1, 8, 4, 11, 8, 3, 2 };

	u32 optionselected[menuitems] = { 0 , 0, videooption, videopatchoption, languageoption+1, hooktypeoption, ocarinaoption, debuggeroption };

	char *start[1] = { "Start" };
	char *videooptions[8] = { "Default Video Mode", "Force NTSC480i", "Force NTSC480p", "Force PAL480i", "Force PAL480p", "Force PAL576i", "Force MPAL480i", "Force MPAL480p" };
	char *videopatchoptions[4] = { "No Video patches", "Smart Video patching", "More Video patching", "Full Video patching" };
	char *languageoptions[11] = { "Default Language", "Japanese", "English", "German", "French", "Spanish", "Italian", "Dutch", "S. Chinese", "T. Chinese", "Korean" };
	char *hooktypeoptions[8] = { "No Ocarina&debugger", "Hooktype: VBI", "Hooktype: KPAD", "Hooktype: Joypad", "Hooktype: GXDraw", "Hooktype: GXFlush", "Hooktype: OSSleepThread", "Hooktype: AXNextFrame" };
	char *ocarinaoptions[3] = { "No Ocarina", "Ocarina from SD", "Ocarina from USB" };
	char *debuggeroptions[2] = { "No debugger", "Debugger enabled" };

	u64 TitleIds[255];
	char *TitleNames[255];

	char **TitleStrings;
	u32 Titlecount;
	
	printf("\nLoading...");

	ret = get_game_list(&TitleStrings, &Titlecount);
	if (ret < 0)
	{
		printf("Error getting the title list\n");
		return;
	}
	if (Titlecount == 0)
	{
		printf("No titles found\n");
		return;
	}
	printf("...");
	
	optioncount[1] = Titlecount;
	char **optiontext[menuitems] = { start, TitleNames, videooptions, videopatchoptions, languageoptions, hooktypeoptions, ocarinaoptions, debuggeroptions };

	for (i = 0; i < Titlecount; i++)
	{
	    TitleIds[i] = TITLE_ID(0x00010001, strtol(TitleStrings[i],NULL,16));
        TitleNames[i] = get_name(TitleIds[i]);		
		printf(".");
	}	

	while (true)
	{
		if (lastbanner != optionselected[1])
		{
			ret = get_tpl_vc(&TexObj, &heighttemp, &widthtemp, TitleIds[optionselected[1]]);
			if (ret < 0)
			{
				waitforbuttonpress(NULL, NULL);
			} else
			{
				//printf("Drawing TPD\n");
				//sleep(5);
				
				//DCFlushRange(&TexObj, sizeof(TexObj));
				gfx_draw_image(200,10, 256,192, TexObj, 0, 1, 1, 0xff);
				//sleep(1);
				//VIDEO_WaitVSync();
				gfx_render_direct();
			}
			lastbanner = optionselected[1];
		}

		printf("\x1b[J");
		
		printheadline();
		printf("\n");
		
		for (i = 0; i < menuitems; i++)
		{
			set_highlight(selection == i);
			if (optiontext[i][optionselected[i]] == NULL)
            {
				printf("???\n");
            } else
			{
				printf("%s\n", optiontext[i][optionselected[i]]);
            }
			set_highlight(false);
		}
		printf("\n");
		
		waitforbuttonpress(&pressed, &pressedGC);
		
		if (pressed == WPAD_BUTTON_UP || pressedGC == PAD_BUTTON_UP)
		{
			if (selection > 0)
			{
				selection--;
			} else
			{
				selection = menuitems-1;
			}
		}

		if (pressed == WPAD_BUTTON_DOWN || pressedGC == PAD_BUTTON_DOWN)
		{
			if (selection < menuitems-1)
			{
				selection++;
			} else
			{
				selection = 0;
			}
		}

		if (pressed == WPAD_BUTTON_LEFT || pressedGC == PAD_BUTTON_LEFT)
		{	
			if (optionselected[selection] > 0)
			{
				optionselected[selection]--;
			} else
			{
				optionselected[selection] = optioncount[selection]-1;
			}
		}

		if (pressed == WPAD_BUTTON_RIGHT || pressedGC == PAD_BUTTON_RIGHT)
		{	
			if (optionselected[selection] < optioncount[selection]-1)
			{
				optionselected[selection]++;
			} else
			{
				optionselected[selection] = 0;
			}
		}

		if (pressed == WPAD_BUTTON_A || pressedGC == PAD_BUTTON_A)
		{
			if (selection == 0)
			{
				videooption = optionselected[2];
				videopatchoption = optionselected[3];
				languageoption = optionselected[4]-1;				
				hooktypeoption = optionselected[5];				
				ocarinaoption = optionselected[6];				
				debuggeroption = optionselected[7];				
				
				bootTitle(TitleIds[optionselected[1]]);
				printf("Press any button to continue\n");
				waitforbuttonpress(NULL, NULL);
			}
		}
		
		if (pressed == WPAD_BUTTON_B || pressedGC == PAD_BUTTON_B)
		{
			printf("Exiting...\n");
			return;
		}	
	}	
}

#define nandmenuitems 1

void show_nand_menu()
{
	int i;
	u32 pressed;
	u32 pressedGC;
	int ret;

	int selection = 0;
	u32 optioncount[nandmenuitems] = { 3 };
	u32 optionselected[nandmenuitems] = { 0 };

	char *nandoptions[3] = { "Use real NAND", "Use SD-NAND", "Use USB-NAND" };
	char **optiontext[nandmenuitems] = { nandoptions };

	while (true)
	{
		printf("\x1b[J");
		
		printheadline();
		printf("\n");
		
		for (i = 0; i < nandmenuitems; i++)
		{
			set_highlight(selection == i);
			if (optiontext[i][optionselected[i]] == NULL)
            {
                printf("???\n");
            } else
			{
				printf("%s\n", optiontext[i][optionselected[i]]);
            }
			set_highlight(false);
		}
		printf("\n");
		
		waitforbuttonpress(&pressed, &pressedGC);
		
		if (pressed == WPAD_BUTTON_UP || pressedGC == PAD_BUTTON_UP)
		{
			if (selection > 0)
			{
				selection--;
			} else
			{
				selection = nandmenuitems-1;
			}
		}

		if (pressed == WPAD_BUTTON_DOWN || pressedGC == PAD_BUTTON_DOWN)
		{
			if (selection < nandmenuitems-1)
			{
				selection++;
			} else
			{
				selection = 0;
			}
		}

		if (pressed == WPAD_BUTTON_LEFT || pressedGC == PAD_BUTTON_LEFT)
		{	
			if (optionselected[selection] > 0)
			{
				optionselected[selection]--;
			} else
			{
				optionselected[selection] = optioncount[selection]-1;
			}
		}

		if (pressed == WPAD_BUTTON_RIGHT || pressedGC == PAD_BUTTON_RIGHT)
		{	
			if (optionselected[selection] < optioncount[selection]-1)
			{
				optionselected[selection]++;
			} else
			{
				optionselected[selection] = 0;
			}
		}

		if (pressed == WPAD_BUTTON_A || pressedGC == PAD_BUTTON_A)
		{
			if (selection == 0)
			{
				ret = 0;
				if (optionselected[0] == 1)
				{
					ret = Enable_Emu(EMU_SD);
				} else
				if (optionselected[0] == 2)
				{
					ret = Enable_Emu(EMU_USB);
				}
				if (ret < 0)
				{
					return;
				}
				
				show_menu();
				return;
			}
		}
		
		if (pressed == WPAD_BUTTON_B || pressedGC == PAD_BUTTON_B)
		{
			printf("Exiting...\n");
			return;
		}	
	}	
}


int main(int argc, char* argv[])
{
	videoInit();

	GX_videoInit__();
	
	DrawBackground(rmode);
	
	Set_Config_to_Defaults();

	printheadline();

	IOS_ReloadIOS(249);

	Power_Flag = false;
	Reset_Flag = false;
	SYS_SetPowerCallback (power_cb);
    SYS_SetResetCallback (reset_cb);

	PAD_Init();
	WPAD_Init();
	WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);	
				

	ISFS_Initialize();

	Set_Config_to_Defaults();
	
	if (IOS_GetVersion() == 249 && IOS_GetRevision() == 14)
	{
		show_nand_menu();
	} else
	{
		show_menu();
	}
	
	printf("Press any button\n");
	waitforbuttonpress(NULL, NULL);
	
	return 0;
}
