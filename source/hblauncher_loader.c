#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <3ds.h>

#include "archive.h"

extern u32 PAYLOAD_TEXTADDR[];
extern u32 PAYLOAD_TEXTMAXSIZE;

extern Handle gspGpuHandle;

u8 *filebuffer;
u32 filebuffer_maxsize;

char regionids_table[7][4] = {//http://3dbrew.org/wiki/Nandrw/sys/SecureInfo_A
"JPN",
"USA",
"EUR",
"JPN", //"AUS"
"CHN",
"KOR",
"TWN"
};

u32 NVer_tidlow_regionarray[7] = {
0x00016202, //JPN
0x00016302, //USA
0x00016102, //EUR
0x00016202, //"AUS"
0x00016402, //CHN
0x00016502, //KOR
0x00016602, //TWN
};

u32 CVer_tidlow_regionarray[7] = {
0x00017202, //JPN
0x00017302, //USA
0x00017102, //EUR
0x00017202, //"AUS"
0x00017402, //CHN
0x00017502, //KOR
0x00017602 //TWN
};

void gxlowcmd_4(u32* inadr, u32* outadr, u32 size, u32 width0, u32 height0, u32 width1, u32 height1, u32 flags)
{
	GX_SetTextureCopy(NULL, inadr, width0 | (height0<<16), outadr, width1 | (height1<<16), size, flags);
}

Result gsp_flushdcache(u8* adr, u32 size)
{
	return GSPGPU_FlushDataCache(NULL, adr, size);
}

Result http_getactual_payloadurl(char *requrl, char *outurl, u32 outurl_maxsize)
{
	Result ret=0;
	httpcContext context;

	ret = httpcOpenContext(&context, requrl, 0);
	if(ret!=0)return ret;

	ret = httpcAddRequestHeaderField(&context, "User-Agent", "hblauncher_loader/"VERSION);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcBeginRequest(&context);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcGetResponseHeader(&context, "Location", outurl, outurl_maxsize);

	httpcCloseContext(&context);

	return 0;
}

Result http_download_payload(char *url, u32 *payloadsize)
{
	Result ret=0;
	u32 statuscode=0;
	u32 contentsize=0;
	httpcContext context;

	ret = httpcOpenContext(&context, url, 0);
	if(ret!=0)return ret;

	ret = httpcAddRequestHeaderField(&context, "User-Agent", "hblauncher_loader/"VERSION);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcBeginRequest(&context);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcGetResponseStatusCode(&context, &statuscode, 0);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	if(statuscode!=200)
	{
		printf("Error: server returned HTTP statuscode %u.\n", (unsigned int)statuscode);
		httpcCloseContext(&context);
		return -2;
	}

	ret=httpcGetDownloadSizeState(&context, NULL, &contentsize);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	if(contentsize==0 || contentsize>PAYLOAD_TEXTMAXSIZE)
	{
		printf("Invalid HTTP content-size: 0x%08x.\n", (unsigned int)contentsize);
		ret = -3;
		httpcCloseContext(&context);
		return ret;
	}

	ret = httpcDownloadData(&context, filebuffer, contentsize, NULL);
	if(ret!=0)
	{
		httpcCloseContext(&context);
		return ret;
	}

	httpcCloseContext(&context);

	*payloadsize = contentsize;

	return 0;
}

Result read_versionbin(FS_archive archive, FS_path fileLowPath, u8 *versionbin)
{
	Result ret = 0;
	Handle filehandle = 0;

	ret = FSUSER_OpenFileDirectly(NULL, &filehandle, archive, fileLowPath, FS_OPEN_READ, 0x0);
	if(ret!=0)
	{
		printf("Failed to open the RomFS image for *Ver: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	ret = romfsInitFromFile(filehandle, 0x0);
	if(ret!=0)
	{
		printf("Failed to mount the RomFS image for *Ver: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	ret = archive_readfile(SDArchive, "romfs:/version.bin", versionbin, 0x8);
	romfsExit();

	if(ret!=0)
	{
		printf("Failed to read the *Ver version.bin: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	return 0;
}

Result load_hblauncher()
{
	Result ret = 0;
	u8 region=0;
	u8 new3dsflag = 0;
	u32 archive_lowpath_data[0x10>>2];//+0 = programID-low, +4 = programID-high, +8 = u8 mediatype.
	u32 file_lowpath_data[0x14>>2];

	FS_archive archive;
	FS_path fileLowPath;

	u8 nver_versionbin[0x8];
	u8 cver_versionbin[0x8];

	u32 payloadsize = 0, payloadsize_aligned = 0;

	char payloadurl[0x80];

	void (*funcptr)(u32*, u32*) = NULL;
	u32 *paramblk = NULL;

	memset(archive_lowpath_data, 0, sizeof(file_lowpath_data));
	memset(file_lowpath_data, 0, sizeof(file_lowpath_data));

	memset(nver_versionbin, 0, sizeof(nver_versionbin));
	memset(cver_versionbin, 0, sizeof(cver_versionbin));

	memset(payloadurl, 0, sizeof(payloadurl));

	archive.id = 0x2345678a;
	archive.lowPath.type = PATH_BINARY;
	archive.lowPath.size = 0x10;
	archive.lowPath.data = (u8*)archive_lowpath_data;

	fileLowPath.type = PATH_BINARY;
	fileLowPath.size = 0x14;
	fileLowPath.data = (u8*)file_lowpath_data;

	archive_lowpath_data[1] = 0x000400DB;

	printf("Getting system-version/system-info etc...\n");

	ret = initCfgu();
	if(ret!=0)
	{
		printf("Failed to init cfgu: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}
	ret = CFGU_SecureInfoGetRegion(&region);
	if(ret!=0)
	{
		printf("Failed to get region from cfgu: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}
	if(region>=7)
	{
		printf("Region value from cfgu is invalid: 0x%02x.\n", (unsigned int)region);
		ret = -9;
		return ret;
	}
	exitCfgu();

	APT_CheckNew3DS(NULL, &new3dsflag);

	archive_lowpath_data[0] = NVer_tidlow_regionarray[region];
	ret = read_versionbin(archive, fileLowPath, nver_versionbin);

	if(ret!=0)
	{
		printf("Failed to read the NVer version.bin: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	archive_lowpath_data[0] = CVer_tidlow_regionarray[region];
	ret = read_versionbin(archive, fileLowPath, cver_versionbin);

	if(ret!=0)
	{
		printf("Failed to read the CVer version.bin: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	snprintf(payloadurl, sizeof(payloadurl)-1, "http://smea.mtheall.com/get_payload.php?version=%s-%d-%d-%d-%d-%s", new3dsflag?"NEW":"OLD", cver_versionbin[2], cver_versionbin[1], cver_versionbin[0], nver_versionbin[2], regionids_table[region]);

	printf("Detected system-version: %s %d.%d.%d-%d %s\n", new3dsflag?"New3DS":"Old3DS", cver_versionbin[2], cver_versionbin[1], cver_versionbin[0], nver_versionbin[2], regionids_table[region]);

	memset(filebuffer, 0, filebuffer_maxsize);

	printf("Checking for the otherapp payload on SD...\n");
	ret = archive_getfilesize(SDArchive, "sdmc:/hblauncherloader_otherapp_payload.bin", &payloadsize);
	if(ret==0)
	{
		if(payloadsize==0 || payloadsize>PAYLOAD_TEXTMAXSIZE)
		{
			printf("Invalid SD payload size: 0x%08x.\n", (unsigned int)payloadsize);
			ret = -3;
		}
	}
	if(ret==0)ret = archive_readfile(SDArchive, "sdmc:/hblauncherloader_otherapp_payload.bin", filebuffer, payloadsize);

	if(ret==0)
	{
		printf("The otherapp payload for this app already exists on SD, that will be used instead of downloading the payload via HTTP.\n");
	}
	else
	{
		printf("Requesting the actual payload URL with HTTP...\n");
		ret = http_getactual_payloadurl(payloadurl, payloadurl, sizeof(payloadurl));
		if(ret!=0)
		{
			printf("Failed to request the actual payload URL: 0x%08x.\n", (unsigned int)ret);
			printf("If the server isn't down, and the HTTP request was actually done, this may mean your system-version or region isn't supported by the hblauncher-payload currently.\n");
			return ret;
		}

		printf("Downloading the actual payload with HTTP...\n");
		ret = http_download_payload(payloadurl, &payloadsize);
		if(ret!=0)
		{
			printf("Failed to download the actual payload with HTTP: 0x%08x.\n", (unsigned int)ret);
			printf("If the server isn't down, and the HTTP request was actually done, this may mean your system-version or region isn't supported by the hblauncher-payload currently.\n");
			return ret;
		}
	}

	printf("Initializing payload data etc...\n");

	payloadsize_aligned = (payloadsize + 0xfff) & ~0xfff;
	if(payloadsize_aligned > PAYLOAD_TEXTMAXSIZE)
	{
		printf("Invalid payload size: 0x%08x.\n", (unsigned int)payloadsize);
		ret = -3;
		return ret;
	}

	memcpy(PAYLOAD_TEXTADDR, filebuffer, payloadsize_aligned);
	memset(filebuffer, 0, filebuffer_maxsize);

	ret = svcFlushProcessDataCache(0xffff8001, PAYLOAD_TEXTADDR, payloadsize_aligned);//Flush dcache for the payload which was copied into .text. Since that area was never executed, icache shouldn't be an issue.
	if(ret!=0)
	{
		printf("svcFlushProcessDataCache failed: 0x%08x.\n", (unsigned int)ret);
		return ret;
	}

	paramblk = linearMemAlign(0x10000, 0x1000);
	if(paramblk==NULL)
	{
		ret = 0xfe;
		printf("Failed to alloc the paramblk.\n");
		return ret;
	}

	httpcExit();

	memset(paramblk, 0, 0x10000);

	paramblk[0x1c>>2] = (u32)gxlowcmd_4;
	paramblk[0x20>>2] = (u32)gsp_flushdcache;
	paramblk[0x48>>2] = 0x8d;//flags
	paramblk[0x58>>2] = &gspGpuHandle;

	printf("Jumping into the payload...\n");

	funcptr = (void*)PAYLOAD_TEXTADDR;
	funcptr(paramblk, (u32*)(0x10000000-0x1000));

	ret = 0xff;
	printf("The payload returned back into the app, this should *never* happen with the actual hblauncher-payload.\n");

	return ret;
}

void displaymessage_waitbutton()
{
	printf("\nPress the A button to continue.\n");
	while(1)
	{
		gspWaitForVBlank();
		hidScanInput();
		if(hidKeysDown() & KEY_A)break;
	}
}

int main(int argc, char **argv)
{
	Result ret = 0;
	u32 pos;
	Handle kproc_handledup=0;

	// Initialize services
	gfxInitDefault();

	consoleInit(GFX_BOTTOM, NULL);

	printf("hblauncher_loader %s by yellows8.\n", VERSION);

	ret = svcDuplicateHandle(&kproc_handledup, 0xffff8001);
	if(ret!=0)printf("svcDuplicateHandle() with the current proc-handle failed: 0x%08x.\n", (unsigned int)ret);

	if(ret==0)
	{
		for(pos=0; pos<PAYLOAD_TEXTMAXSIZE; pos+=0x1000)
		{
			ret = svcControlProcessMemory(kproc_handledup, (u32)&PAYLOAD_TEXTADDR[pos >> 2], 0x0, 0x1000, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
			if(ret!=0)
			{
				printf("svcControlProcessMemory with pos=0x%x failed: 0x%08x.\n", (unsigned int)pos, (unsigned int)ret);
				break;
			}
		}
	}

	ret = httpcInit();
	if(ret!=0)
	{
		printf("Failed to initialize HTTPC: 0x%08x.\n", (unsigned int)ret);
		if(ret==0xd8e06406)
		{
			printf("The HTTPC service is inaccessible.\n");
		}
	}

	if(ret==0)
	{
		filebuffer_maxsize = PAYLOAD_TEXTMAXSIZE;

		filebuffer = (u8*)malloc(filebuffer_maxsize);
		if(filebuffer==NULL)
		{
			printf("Failed to allocate memory.\n");
			ret = -1;
		}
		else
		{
			memset(filebuffer, 0, filebuffer_maxsize);
		}
	}

	ret = load_hblauncher();

	free(filebuffer);

	httpcExit();

	if(ret!=0)printf("An error occured, please report this to here if it persists(or comment on an already existing issue if needed), with an image of your 3DS system with the bottom-screen: https://github.com/yellows8/hblauncher_loader/issues\n");

	printf("Press the START button to exit.\n");
	// Main loop
	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu
	}

	// Exit services
	gfxExit();
	return 0;
}

