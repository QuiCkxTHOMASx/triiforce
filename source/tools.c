#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <wiiuse/wpad.h>

#include "tools.h"

#define TRIIFORCE_MAJOR 0
#define TRIIFORCE_MINOR 2

void printheadline()
{
	int rows, cols;
	CON_GetMetrics(&cols, &rows);

	printf("TriiForce %i.%i", TRIIFORCE_MAJOR, TRIIFORCE_MINOR);
	
	char buf[64];
	sprintf(buf, "IOS%u (Rev %u)\n", IOS_GetVersion(), IOS_GetRevision());
	printf("\x1B[%d;%dH", 0, cols-strlen(buf)-1);	
	printf(buf);
}

void set_highlight(bool highlight)
{
	if (highlight)
	{
		printf("\x1b[%u;%um", 47, false);
		printf("\x1b[%u;%um", 30, false);
	} else
	{
		printf("\x1b[%u;%um", 37, false);
		printf("\x1b[%u;%um", 40, false);
	}
}

void *allocate_memory(u32 size)
{
	return memalign(32, (size+31)&(~31) );
}

void waitforbuttonpress(u32 *out, u32 *outGC)
{
	u32 pressed = 0;
	u32 pressedGC = 0;

	while (true)
	{
		WPAD_ScanPads();
		pressed = WPAD_ButtonsDown(0);

		PAD_ScanPads();
		pressedGC = PAD_ButtonsDown(0);

		if(pressed || pressedGC) 
		{
			if (pressedGC)
			{
				// Without waiting you can't select anything
				usleep (20000);
			}
			if (out) *out = pressed;
			if (outGC) *outGC = pressedGC;
			return;
		}
	}
}


s32 read_file(char *filepath, u8 **buffer, u32 *filesize)
{
	s32 Fd;
	int ret;

	if (buffer == NULL)
	{
		printf("NULL Pointer\n");
		return -1;
	}

	Fd = ISFS_Open(filepath, ISFS_OPEN_READ);
	if (Fd < 0)
	{
		printf("ISFS_Open %s failed %d\n", filepath, Fd);
		return Fd;
	}

	fstats *status;
	status = allocate_memory(sizeof(fstats));
	if (status == NULL)
	{
		printf("Out of memory for status\n");
		return -1;
	}
	
	ret = ISFS_GetFileStats(Fd, status);
	if (ret < 0)
	{
		printf("ISFS_GetFileStats failed %d\n", ret);
		ISFS_Close(Fd);
		free(status);
		return -1;
	}
	
	*buffer = allocate_memory(status->file_length);
	if (*buffer == NULL)
	{
		printf("Out of memory for buffer\n");
		ISFS_Close(Fd);
		free(status);
		return -1;
	}
		
	ret = ISFS_Read(Fd, *buffer, status->file_length);
	if (ret < 0)
	{
		printf("ISFS_Read failed %d\n", ret);
		ISFS_Close(Fd);
		free(status);
		free(*buffer);	
	}
	ISFS_Close(Fd);

	*filesize = status->file_length;
	free(status);

	return 0;
}

s32 identify(u64 titleid, u16 *ios)
{
	char filepath[ISFS_MAXPATH] ATTRIBUTE_ALIGN(0x20);
	u8 *tmdBuffer = NULL;
	u32 tmdSize;
	u8 *tikBuffer = NULL;
	u32 tikSize;
	u8 *certBuffer = NULL;
	u32 certSize;
	
	int ret;

	printf("Reading TMD...");
	fflush(stdout);
	
	sprintf(filepath, "/title/%08x/%08x/content/title.tmd", TITLE_UPPER(titleid), TITLE_LOWER(titleid));
	ret = read_file(filepath, &tmdBuffer, &tmdSize);
	if (ret < 0)
	{
		printf("Reading TMD failed\n");
		return ret;
	}
	printf("done\n");

	*ios = (u32)(tmdBuffer[0x18b]);

	printf("Reading ticket...");
	fflush(stdout);

	sprintf(filepath, "/ticket/%08x/%08x.tik", TITLE_UPPER(titleid), TITLE_LOWER(titleid));
	ret = read_file(filepath, &tikBuffer, &tikSize);
	if (ret < 0)
	{
		printf("Reading ticket failed\n");
		free(tmdBuffer);
		return ret;
	}
	printf("done\n");

	printf("Reading certs...");
	fflush(stdout);

	sprintf(filepath, "/sys/cert.sys");
	ret = read_file(filepath, &certBuffer, &certSize);
	if (ret < 0)
	{
		printf("Reading certs failed\n");
		free(tmdBuffer);
		free(tikBuffer);
		return ret;
	}
	printf("done\n");
	
	printf("ES_Identify...");
	fflush(stdout);

	ret = ES_Identify((signed_blob*)certBuffer, certSize, (signed_blob*)tmdBuffer, tmdSize, (signed_blob*)tikBuffer, tikSize, NULL);
	if (ret < 0)
	{
		switch(ret)
		{
			case ES_EINVAL:
				printf("Error! ES_Identify (ret = %d;) Data invalid!\n", ret);
				break;
			case ES_EALIGN:
				printf("Error! ES_Identify (ret = %d;) Data not aligned!\n", ret);
				break;
			case ES_ENOTINIT:
				printf("Error! ES_Identify (ret = %d;) ES not initialized!\n", ret);
				break;
			case ES_ENOMEM:
				printf("Error! ES_Identify (ret = %d;) No memory!\n", ret);
				break;
			default:
				printf("Error! ES_Identify (ret = %d)\n", ret);
				break;
		}
		free(tmdBuffer);
		free(tikBuffer);
		free(certBuffer);
		return ret;
	}
	printf("done\n");
	
	free(tmdBuffer);
	free(tikBuffer);
	free(certBuffer);
	return 0;
}

s32 parser(u8 *file, u32 size, u8 *value, u8 *patch, u32 valuesize, u32 patchsize, u32 offset)
{  
    int i = 0;
    u32 cnt = 0;
    u32 maxsize = size - valuesize;
	if (patchsize + offset > valuesize)
	{
		maxsize = size - patchsize - offset;
	}
	
	for (i = 0; i < maxsize; i++)
	{
        if (memcmp(file+i, value, valuesize) == 0) 
		{
            memcpy(file+i+offset, patch, patchsize);
            cnt++;
			i = i + offset + patchsize;
        }
	} 
	
	if (cnt > 0)
	{
		return cnt;
	} else
	{
		return -1;
	}
}

