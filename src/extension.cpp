#include "extension.h"


IGameConfig *g_pGameConf = NULL;

Cleaner g_Cleaner;
SMEXT_LINK(&g_Cleaner);

CDetour *g_pDetour = 0;

char ** g_szStrings;
int g_iStrings = 0;

IForward *g_Forward = nullptr;

ke::ThreadId Thread_Id;


#if SOURCE_ENGINE >= SE_LEFT4DEAD2
DETOUR_DECL_MEMBER4(Detour_LogDirect, LoggingResponse_t, LoggingChannelID_t, channelID, LoggingSeverity_t, severity, Color, color, const tchar *, pMessage)
{
	if (Thread_Id == ke::GetCurrentThreadId())
	{
		cell_t res = PLUGIN_CONTINUE;

		g_Forward->PushString(pMessage);
		g_Forward->Execute(&res);
		if (res != Pl_Continue) {
			return LR_CONTINUE;
		}
	}

	for(int i=0;i<g_iStrings;++i)
		if(strstr(pMessage, g_szStrings[i])!=0)
			return LR_CONTINUE;
	return DETOUR_MEMBER_CALL(Detour_LogDirect)(channelID, severity, color, pMessage);
}

#ifdef PLATFORM_WINDOWS
//https://github.com/alliedmodders/sourcemod/blob/237db0504c7a59e394828446af3e8ca3d53ef647/extensions/sdktools/vglobals.cpp#L149
size_t UTIL_StringToSignature(const char *str, char buffer[], size_t maxlength)
{
	size_t real_bytes = 0;
	size_t length = strlen(str);

	for (size_t i=0; i<length; i++)
	{
		if (real_bytes >= maxlength)
		{
			break;
		}
		buffer[real_bytes++] = (unsigned char)str[i];
		if (str[i] == '\\'
			&& str[i+1] == 'x')
		{
			if (i + 3 >= length)
			{
				continue;
			}
			/* Get the hex part */
			char s_byte[3];
			int r_byte;
			s_byte[0] = str[i+2];
			s_byte[1] = str[i+3];
			s_byte[2] = '\n';
			/* Read it as an integer */
			sscanf(s_byte, "%x", &r_byte);
			/* Save the value */
			buffer[real_bytes-1] = (unsigned char)r_byte;
			/* Adjust index */
			i += 3;
		}
	}

	return real_bytes;
}
#endif
#else
DETOUR_DECL_STATIC2(Detour_DefSpew, SpewRetval_t, SpewType_t, channel, char *, text)
{

	for(int i=0;i<g_iStrings;++i)
		if(strstr(text, g_szStrings[i])!=0)
			return SPEW_CONTINUE;
	return DETOUR_STATIC_CALL(Detour_DefSpew)(channel, text);
}

#endif

bool Cleaner::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	g_Forward = forwards->CreateForward("OnServerConsolePrint", ET_Event, 1, NULL, Param_String);
	Thread_Id = ke::GetCurrentThreadId();

	CDetourManager::Init(g_pSM->GetScriptingEngine(), 0);

	char szPath[256];
	g_pSM->BuildPath(Path_SM, szPath, sizeof(szPath), "configs/cleaner.cfg");
	FILE * file = fopen(szPath, "r");

	if(file==NULL)
	{
		snprintf(error, maxlength, "Could not read configs/cleaner.cfg.");
		return false;
	}

	int c, lines = 0;
	do
	{
		c = fgetc(file);
		++lines;
	} while (c != EOF);

	rewind(file);

	int len;
	g_szStrings = (char**)malloc(lines*sizeof(char**));
	while(!feof(file))
	{
		g_szStrings[g_iStrings] = (char*)malloc(256*sizeof(char*));
		if (fgets(g_szStrings[g_iStrings], 255, file) != NULL)
		{
			len = strlen(g_szStrings[g_iStrings]);
			if(g_szStrings[g_iStrings][len-1]=='\r' || g_szStrings[g_iStrings][len-1]=='\n')
					g_szStrings[g_iStrings][len-1]=0;
			if(g_szStrings[g_iStrings][len-2]=='\r')
					g_szStrings[g_iStrings][len-2]=0;
			++g_iStrings;
		}
	}
	fclose(file);

#if SOURCE_ENGINE >= SE_LEFT4DEAD2
	char ConfigError[128];
	if(!gameconfs->LoadGameConfigFile("cleaner", &g_pGameConf, ConfigError, sizeof(ConfigError)))
	{
		if (error)
		{
			snprintf(error, maxlength, "cleaner.txt error : %s", ConfigError);
		}
		return false;
	}
	
#ifdef PLATFORM_WINDOWS
	HMODULE tier0 = GetModuleHandle("tier0.dll");
	char sig[256];
	size_t size = UTIL_StringToSignature(g_pGameConf->GetKeyValue("ServerConsolePrintSig_windows"), sig, sizeof(sig));
	void * fn = memutils->FindPattern(tier0, sig, size);
#elif defined PLATFORM_LINUX
#if SOURCE_ENGINE == SE_LEFT4DEAD2
	void * tier0 = dlopen("libtier0_srv.so", RTLD_NOW);
#else
	void * tier0 = dlopen("libtier0.so", RTLD_NOW);
#endif
#if SOURCE_ENGINE == SE_CSGO
	void * fn = dlsym(tier0, g_pGameConf->GetKeyValue("ServerConsolePrintSig_linux"));
#else
	void * fn = memutils->ResolveSymbol(tier0, g_pGameConf->GetKeyValue("ServerConsolePrintSig_linux"));
#endif
	dlclose(tier0);
#else
	#error "Unsupported OS"
#endif
	if(!fn)
	{
		snprintf(error, maxlength, "Failed to find signature. Please contact the author.");
		return false;
	}
#if SOURCE_ENGINE == SE_CSGO
#ifdef PLATFORM_LINUX
	int offset = 0;
	if (!g_pGameConf->GetOffset("ServerConsolePrint", &offset))
	{
		snprintf(error, maxlength, "Failed to get ServerConsolePrint offset.");
		return false;
	}

	fn = (void *)((intptr_t)fn + offset);
#endif
#endif
	g_pDetour = DETOUR_CREATE_MEMBER(Detour_LogDirect, fn);
#else
	g_pDetour = DETOUR_CREATE_STATIC(Detour_DefSpew, (gpointer)GetSpewOutputFunc());
#endif
	
	if (g_pDetour == NULL)
	{
		snprintf(error, maxlength, "Failed to initialize the detours. Please contact the author.");
		return false;
	}

	g_pDetour->EnableDetour();

	return true;
}

void Cleaner::SDK_OnUnload()
{
	forwards->ReleaseForward(g_Forward);

	if(g_pDetour)
		g_pDetour->Destroy();

	delete [] g_szStrings;
#if SOURCE_ENGINE >= SE_LEFT4DEAD2
	gameconfs->CloseGameConfigFile(g_pGameConf);
#endif
}
