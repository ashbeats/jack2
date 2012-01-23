/*
 Copyright (C) 2006-2011 Grame

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files
 (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge,
 publish, distribute, sublicense, and/or sell copies of the Software,
 and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be
 included in all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#ifdef WIN32
#pragma warning (disable : 4786)
#endif

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <process.h>
#include "JackRouter.h"
#include "profport.h"

/*
	08/07/2007 SL : Use jack_client_open instead of jack_client_new (automatic client renaming).
	09/08/2007 SL : Add JackRouter.ini parameter file.
	09/20/2007 SL : Better error report in DllRegisterServer (for Vista).
	09/27/2007 SL : Add AUDO_CONNECT property in JackRouter.ini file.
	10/10/2007 SL : Use ASIOSTInt32LSB instead of ASIOSTInt16LSB.
	12/04/2011 SL : Compilation on Windows 64.
	12/04/2011 SL : Dynamic port allocation. Correct JACK port naming.
 */

//------------------------------------------------------------------------------------------
// extern
void getNanoSeconds(ASIOTimeStamp *time);

// local
double AsioSamples2double (ASIOSamples* samples);

static const double twoRaisedTo32 = 4294967296.;
static const double twoRaisedTo32Reciprocal = 1. / twoRaisedTo32;

//------------------------------------------------------------------------------------------
// on windows, we do the COM stuff.

#if WINDOWS
#include "windows.h"
#include "mmsystem.h"
#ifdef _WIN64
#define JACK_ROUTER "JackRouter.dll"
#include <psapi.h>
#else
#define JACK_ROUTER "JackRouter.dll"
#include "./psapi.h"
#endif

using namespace std;

// class id.
// {838FE50A-C1AB-4b77-B9B6-0A40788B53F3}
CLSID IID_ASIO_DRIVER = { 0x838fe50a, 0xc1ab, 0x4b77, { 0xb9, 0xb6, 0xa, 0x40, 0x78, 0x8b, 0x53, 0xf3 } };


CFactoryTemplate g_Templates[1] = {
    {L"ASIOJACK", &IID_ASIO_DRIVER, JackRouter::CreateInstance}
};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

CUnknown* JackRouter::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
	return (CUnknown*)new JackRouter(pUnk,phr);
};

STDMETHODIMP JackRouter::NonDelegatingQueryInterface(REFIID riid, void ** ppv)
{
	if (riid == IID_ASIO_DRIVER) {
		return GetInterface(this, ppv);
	}
	return CUnknown::NonDelegatingQueryInterface(riid, ppv);
}

// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//		Register ASIO Driver
// ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
extern LONG RegisterAsioDriver(CLSID,char *,char *,char *,char *);
extern LONG UnregisterAsioDriver(CLSID,char *,char *);

//
// Server registration, called on REGSVR32.EXE "the dllname.dll"
//
HRESULT _stdcall DllRegisterServer()
{
	LONG	rc;
	char	errstr[128];

	rc = RegisterAsioDriver (IID_ASIO_DRIVER, JACK_ROUTER,"JackRouter","JackRouter","Apartment");

	if (rc) {
		memset(errstr,0,128);
		sprintf(errstr,"Register Server failed ! (%d)", rc);
		MessageBox(0,(LPCTSTR)errstr,(LPCTSTR)"JackRouter",MB_OK);
		return -1;
	}

	return S_OK;
}

//
// Server unregistration
//
HRESULT _stdcall DllUnregisterServer()
{
	LONG	rc;
	char	errstr[128];

	rc = UnregisterAsioDriver (IID_ASIO_DRIVER,JACK_ROUTER,"JackRouter");

	if (rc) {
		memset(errstr,0,128);
		sprintf(errstr,"Unregister Server failed ! (%d)",rc);
		MessageBox(0,(LPCTSTR)errstr,(LPCTSTR)"JackRouter",MB_OK);
		return -1;
	}

	return S_OK;
}

// Globals

list<pair<string, string> > JackRouter::fConnections;


//------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------
JackRouter::JackRouter (LPUNKNOWN pUnk, HRESULT *phr)
	: CUnknown("ASIOJACK", pUnk, phr)

//------------------------------------------------------------------------------------------

#else

// when not on windows, we derive from AsioDriver
JackRouter::JackRouter() : AsioDriver()

#endif
{
	long i;
	fSamplePosition = 0;
	fActive = false;
	fStarted = false;
	fTimeInfoMode = false;
	fTcRead = false;
	fClient = NULL;
	fAutoConnectIn = true;
	fAutoConnectOut = true;
	fCallbacks = 0;
	fActiveInputs = fActiveOutputs = 0;
	fToggle = 0;
	fBufferSize = 512;
	fSampleRate = 44100;
    fFloatSample = true;    // float by default
	fFirstActivate = true; 
	
    printf("Constructor\n");

	// Use "jackrouter.ini" parameters if available
	HMODULE handle = LoadLibrary(JACK_ROUTER);

	if (handle) {

		// Get JackRouter.dll path
		char dllName[512];
		string confPath;
		DWORD res = GetModuleFileName(handle, dllName, 512);

		// Compute .ini file path
		string fullPath = dllName;
		int lastPos = fullPath.find_last_of(PATH_SEP);
		string  dllFolder =  fullPath.substr(0, lastPos);
		confPath = dllFolder + PATH_SEP + "JackRouter.ini";

		// Get parameters
		kNumInputs = get_private_profile_int("IO", "input", 2, confPath.c_str());
		kNumOutputs = get_private_profile_int("IO", "output", 2, confPath.c_str());

		fAutoConnectIn = get_private_profile_int("AUTO_CONNECT", "input", 1, confPath.c_str());
		fAutoConnectOut = get_private_profile_int("AUTO_CONNECT", "output", 1, confPath.c_str());
        
        fFloatSample = get_private_profile_int("IO", "float-sample", 0, confPath.c_str());
        
        fAliasSystem = get_private_profile_int("AUTO_CONNECT", "alias", 0, confPath.c_str());

		FreeLibrary(handle);

	} else {
		printf("LoadLibrary error\n");
	}
    
    if (!fFloatSample) {
        fInputBuffers = (void**)new long*[kNumInputs];
        fOutputBuffers = (void**)new long*[kNumOutputs];
    } else {
        fInputBuffers = (void**)new float*[kNumInputs];
        fOutputBuffers = (void**)new float*[kNumOutputs];
    }

    fInMap = new long[kNumInputs];
	fOutMap = new long[kNumOutputs];
    
    fInputPorts = new jack_port_t*[kNumInputs];
    fOutputPorts = new jack_port_t*[kNumOutputs];
 
	for (i = 0; i < kNumInputs; i++) {
		fInputBuffers[i] = 0;
		fInputPorts[i] = 0;
		fInMap[i] = 0;
	}
	for (i = 0; i < kNumOutputs; i++) {
		fOutputBuffers[i] = 0;
		fOutputPorts[i] = 0;
		fOutMap[i] = 0;
	}
}

//------------------------------------------------------------------------------------------
JackRouter::~JackRouter()
{
    printf("Destructor\n");
	stop ();
	disposeBuffers ();
	jack_client_close(fClient);
	delete[] fInputBuffers;
    delete[] fOutputBuffers;
    delete[] fInputPorts;
    delete[] fOutputPorts;
    delete[] fInMap;
    delete[] fOutMap;
}

//------------------------------------------------------------------------------------------

static bool GetEXEName(DWORD dwProcessID, char* name)
{
    DWORD aProcesses [1024], cbNeeded, cProcesses;
    unsigned int i;

    // Enumerate all processes
    if (!EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded))
        return false;

    // Calculate how many process identifiers were returned.
    cProcesses = cbNeeded / sizeof(DWORD);

    TCHAR szEXEName[MAX_PATH];
    // Loop through all process to find the one that matches
    // the one we are looking for

    for (i = 0; i < cProcesses; i++) {
        if (aProcesses [i] == dwProcessID) {
            // Get a handle to the process
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_READ, FALSE, dwProcessID);

            // Get the process name
            if (NULL != hProcess) {
                HMODULE hMod;
                DWORD cbNeeded;

                if (EnumProcessModules(hProcess, &hMod,
                                      sizeof(hMod), &cbNeeded)) {
                    //Get the name of the exe file
                    GetModuleBaseName(hProcess, hMod, szEXEName,
                        sizeof(szEXEName)/sizeof(TCHAR));
					int len = strlen((char*)szEXEName) - 4; // remove ".exe"
					strncpy(name, (char*)szEXEName, len);
					name[len] = '\0';
					return true;
                 }
            }
        }
    }

    return false;
}

 //------------------------------------------------------------------------------------------
static inline jack_default_audio_sample_t ClipFloat(jack_default_audio_sample_t sample)
{
     return (sample < jack_default_audio_sample_t(-1.0)) ? jack_default_audio_sample_t(-1.0) : (sample > jack_default_audio_sample_t(1.0)) ? jack_default_audio_sample_t(1.0) : sample;
}

//------------------------------------------------------------------------------------------
void JackRouter::connectCallback(jack_port_id_t a, jack_port_id_t b, int connect, void* arg)
{
    JackRouter* driver = (JackRouter*)arg;
}

//------------------------------------------------------------------------------------------
void JackRouter::shutdownCallback(void* arg)
{
	JackRouter* driver = (JackRouter*)arg;
	/*
	char errstr[128];
	memset(errstr,0,128);
	sprintf(errstr,"JACK server has quitted");
	MessageBox(0,(LPCTSTR)errstr,(LPCTSTR)"JackRouter",MB_OK);
	*/
}

//------------------------------------------------------------------------------------------
int JackRouter::processCallback(jack_nframes_t nframes, void* arg)
{
	JackRouter* driver = (JackRouter*)arg;
	int i,j;
	int pos = (driver->fToggle) ? 0 : driver->fBufferSize ;

	for (i = 0; i < driver->fActiveInputs; i++) {
        if (!driver->fFloatSample) {
            jack_default_audio_sample_t* buffer = (jack_default_audio_sample_t*)jack_port_get_buffer(driver->fInputPorts[i], nframes);
            long* in = (long*)driver->fInputBuffers[i] + pos;
            for (j = 0; j < nframes; j++) {
                in[j] = buffer[j] * jack_default_audio_sample_t(0x7fffffff);
            }
        } else {
            memcpy((float*)driver->fInputBuffers[i] + pos,
                    jack_port_get_buffer(driver->fInputPorts[i], nframes),
                    nframes * sizeof(jack_default_audio_sample_t));
        }
	}

	driver->bufferSwitch();

	for (i = 0; i < driver->fActiveOutputs; i++) {
        if (!driver->fFloatSample) {
            jack_default_audio_sample_t* buffer = (jack_default_audio_sample_t*)jack_port_get_buffer(driver->fOutputPorts[i], nframes);
            long* out = (long*)driver->fOutputBuffers[i] + pos;
            jack_default_audio_sample_t gain = jack_default_audio_sample_t(1)/jack_default_audio_sample_t(0x7fffffff);
            for (j = 0; j < nframes; j++) {
                buffer[j] = out[j] * gain;
            }
        } else {
            memcpy(jack_port_get_buffer(driver->fOutputPorts[i], nframes),
                    (float*)driver->fOutputBuffers[i] + pos,
                    nframes * sizeof(jack_default_audio_sample_t));
        }
	}

	return 0;
}

//------------------------------------------------------------------------------------------
void JackRouter::getDriverName(char *name)
{
	strcpy (name, "JackRouter");
}

//------------------------------------------------------------------------------------------
long JackRouter::getDriverVersion()
{
	return 0x00000001L;
}

//------------------------------------------------------------------------------------------
void JackRouter::getErrorMessage(char *string)
{
	strcpy (string, fErrorMessage);
}

//------------------------------------------------------------------------------------------
ASIOBool JackRouter::init(void* sysRef)
{
	char name[MAX_PATH];
	sysRef = sysRef;

	if (fActive)
		return true;

	HANDLE win = (HANDLE)sysRef;
	int	my_pid = _getpid();

	if (!GetEXEName(my_pid, name)) { // If getting the .exe name fails, takes a generic one.
		_snprintf(name, sizeof(name) - 1, "JackRouter_%d", my_pid);
	}

	if (fClient) {
		printf("Error: JACK client still present...\n");
		return true;
	}

	fClient = jack_client_open(name, JackNullOption, NULL);
	if (fClient == NULL) {
		strcpy(fErrorMessage, "Open error: is JACK server running?");
		printf("Open error: is JACK server running?\n");
		return false;
	}

	fBufferSize = jack_get_buffer_size(fClient);
	fSampleRate = jack_get_sample_rate(fClient);
    
	jack_set_process_callback(fClient, processCallback, this);
	jack_on_shutdown(fClient, shutdownCallback, this);
    jack_set_port_connect_callback(fClient, connectCallback, this);

	fInputLatency = fBufferSize;		// typically
	fOutputLatency = fBufferSize * 2;
	fMilliSeconds = (long)((double)(fBufferSize * 1000) / fSampleRate);

	// Typically fBufferSize * 2; try to get 1 by offering direct buffer
	// access, and using asioPostOutput for lower latency

	printf("Init ASIO JACK\n");
	fActive = true;
	return true;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::start()
{
	if (fCallbacks) {
		fSamplePosition = 0;
		fTheSystemTime.lo = fTheSystemTime.hi = 0;
		fToggle = 0;
		fStarted = true;
		printf("Start ASIO JACK\n");

		if (jack_activate(fClient) == 0) {

			if (fFirstActivate) {
				AutoConnect();
				fFirstActivate = false;
			} else {
				RestoreConnections();
			}

			return ASE_OK;

		} else {
			return ASE_NotPresent;
		}
	}

	return ASE_NotPresent;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::stop()
{
	printf("Stop ASIO JACK\n");
	fStarted = false;
	SaveConnections();
	jack_deactivate(fClient);
	return ASE_OK;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::getChannels(long *numInputChannels, long *numOutputChannels)
{
	*numInputChannels = kNumInputs;
	*numOutputChannels = kNumOutputs;
	return ASE_OK;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::getLatencies(long *_inputLatency, long *_outputLatency)
{
	*_inputLatency = fInputLatency;
	*_outputLatency = fOutputLatency;
	return ASE_OK;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::getBufferSize(long *minSize, long *maxSize, long *preferredSize, long *granularity)
{
	*minSize = *maxSize = *preferredSize = fBufferSize;		// Allows this size only
	*granularity = 0;
	return ASE_OK;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::canSampleRate(ASIOSampleRate sampleRate)
{
	return (sampleRate == fSampleRate) ? ASE_OK : ASE_NoClock;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::getSampleRate(ASIOSampleRate *sampleRate)
{
	*sampleRate = fSampleRate;
	return ASE_OK;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::setSampleRate(ASIOSampleRate sampleRate)
{
	return (sampleRate == fSampleRate) ? ASE_OK : ASE_NoClock;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::getClockSources(ASIOClockSource *clocks, long *numSources)
{
	// Internal
	if (clocks && numSources) {
		clocks->index = 0;
		clocks->associatedChannel = -1;
		clocks->associatedGroup = -1;
		clocks->isCurrentSource = ASIOTrue;
		strcpy(clocks->name, "Internal");
		*numSources = 1;
		return ASE_OK;
	} else {
		return ASE_InvalidParameter;
	}
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::setClockSource(long index)
{
	if (!index) {
		fAsioTime.timeInfo.flags |= kClockSourceChanged;
		return ASE_OK;
	} else {
		return ASE_NotPresent;
	}
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp)
{
	tStamp->lo = fTheSystemTime.lo;
	tStamp->hi = fTheSystemTime.hi;

	if (fSamplePosition >= twoRaisedTo32) {
		sPos->hi = (unsigned long)(fSamplePosition * twoRaisedTo32Reciprocal);
		sPos->lo = (unsigned long)(fSamplePosition - (sPos->hi * twoRaisedTo32));
	} else {
		sPos->hi = 0;
		sPos->lo = (unsigned long)fSamplePosition;
	}
	return ASE_OK;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::getChannelInfo(ASIOChannelInfo *info)
{
	if (info->channel < 0 || (info->isInput ? info->channel >= kNumInputs : info->channel >= kNumOutputs)) {
		return ASE_InvalidParameter;
    }
    
    if (!fFloatSample) {
        info->type = ASIOSTInt32LSB;
    } else {
        info->type = ASIOSTFloat32LSB;
    }

	info->channelGroup = 0;
	info->isActive = ASIOFalse;
	long i;
	char buf[32];
    const char** ports;

	char* aliases[2];
	aliases[0] = (char*)malloc(jack_port_name_size());
	aliases[1] = (char*)malloc(jack_port_name_size());

	if (!aliases[0] || !aliases[1]) {
		return ASE_NoMemory;
    }

	if (info->isInput) {
		for (i = 0; i < fActiveInputs; i++) {
			if (fInMap[i] == info->channel) {
				info->isActive = ASIOTrue;
				break;
			}
		}
		
        // A alias on system is wanted
        if (fAliasSystem && fAutoConnectIn && (ports = jack_get_ports(fClient, NULL, NULL, JackPortIsPhysical | JackPortIsOutput))) {
            jack_port_t* port = jack_port_by_name(fClient, ports[info->channel]);
            if (port) {	
                if (jack_port_get_aliases(port, aliases) == 2) {
                    strncpy(info->name, aliases[1], 32);
                    goto end:
                }	
            }
        } 
            
        _snprintf(buf, sizeof(buf) - 1, "In%d", info->channel);
        strcpy(info->name, buf);
         
	} else {
		for (i = 0; i < fActiveOutputs; i++) {
			if (fOutMap[i] == info->channel) {  
				info->isActive = ASIOTrue;
				break;
			}
		}
        
        // A alias on system is wanted
        if (fAliasSystem && fAutoConnectOut && (ports = jack_get_ports(fClient, NULL, NULL, JackPortIsPhysical | JackPortIsInput))) {
            jack_port_t* port = jack_port_by_name(fClient, ports[info->channel]);
            if (port) {	
                if (jack_port_get_aliases(port, aliases) == 2) {
                    strncpy(info->name, aliases[1], 32);
                    goto end:
                }	
            }
        } 
        _snprintf(buf, sizeof(buf) - 1, "Out%d", info->channel);
        strcpy(info->name, buf);
    }
    
end:

	free(aliases[0]);
	free(aliases[1]);
	return ASE_OK;
}

//------------------------------------------------------------------------------------------
ASIOError JackRouter::createBuffers(ASIOBufferInfo *bufferInfos, long numChannels,
	long bufferSize, ASIOCallbacks *callbacks)
{
	ASIOBufferInfo *info = bufferInfos;
	long i;
	bool notEnoughMem = false;
	char buf[256];
	fActiveInputs = 0;
	fActiveOutputs = 0;

	for (i = 0; i < numChannels; i++, info++) {
		if (info->isInput) {
			if (info->channelNum < 0 || info->channelNum >= kNumInputs)
				goto error;
			fInMap[fActiveInputs] = info->channelNum;
            if (!fFloatSample) {
                fInputBuffers[fActiveInputs] = new long[fBufferSize * 2];	// double buffer
            } else {
                fInputBuffers[fActiveInputs] = new jack_default_audio_sample_t[fBufferSize * 2];	// double buffer
            }
			if (fInputBuffers[fActiveInputs]) {
				info->buffers[0] = fInputBuffers[fActiveInputs];
				info->buffers[1] = (fFloatSample) ? (void*)((float*)fInputBuffers[fActiveInputs] + fBufferSize) : (void*)((long*)fInputBuffers[fActiveInputs] + fBufferSize);
			} else {
				info->buffers[0] = info->buffers[1] = 0;
				notEnoughMem = true;
			}

			_snprintf(buf, sizeof(buf) - 1, "in%d", info->channelNum + 1);
			fInputPorts[fActiveInputs]
				= jack_port_register(fClient, buf, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,0);
			if (fInputPorts[fActiveInputs] == NULL)
				goto error;

			fActiveInputs++;
			if (fActiveInputs > kNumInputs) {
error:
				disposeBuffers();
				return ASE_InvalidParameter;
			}
		} else {	// output
			if (info->channelNum < 0 || info->channelNum >= kNumOutputs)
				goto error;
			fOutMap[fActiveOutputs] = info->channelNum;

            if (!fFloatSample) {
                fOutputBuffers[fActiveOutputs] = new long[fBufferSize * 2];	// double buffer
            } else {
                fOutputBuffers[fActiveOutputs] = new jack_default_audio_sample_t[fBufferSize * 2];	// double buffer
            }

			if (fOutputBuffers[fActiveOutputs]) {
				info->buffers[0] = fOutputBuffers[fActiveOutputs];
				info->buffers[1] = (fFloatSample) ? (void*)((float*)fOutputBuffers[fActiveOutputs] + fBufferSize) : (void*)((long*)fOutputBuffers[fActiveOutputs] + fBufferSize);
			} else {
				info->buffers[0] = info->buffers[1] = 0;
				notEnoughMem = true;
			}
			
			_snprintf(buf, sizeof(buf) - 1, "out%d", info->channelNum + 1);
			fOutputPorts[fActiveOutputs]
				= jack_port_register(fClient, buf, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput,0);
			if (fOutputPorts[fActiveOutputs] == NULL)
				goto error;

			fActiveOutputs++;
			if (fActiveOutputs > kNumOutputs) {
				fActiveOutputs--;
				disposeBuffers();
				return ASE_InvalidParameter;
			}
		}
	}

	if (notEnoughMem) {
		disposeBuffers();
		return ASE_NoMemory;
	}

	this->fCallbacks = callbacks;
	if (callbacks->asioMessage (kAsioSupportsTimeInfo, 0, 0, 0)) {
		fTimeInfoMode = true;
		fAsioTime.timeInfo.speed = 1.;
		fAsioTime.timeInfo.systemTime.hi = fAsioTime.timeInfo.systemTime.lo = 0;
		fAsioTime.timeInfo.samplePosition.hi = fAsioTime.timeInfo.samplePosition.lo = 0;
		fAsioTime.timeInfo.sampleRate = fSampleRate;
		fAsioTime.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
		fAsioTime.timeCode.speed = 1.;
		fAsioTime.timeCode.timeCodeSamples.lo = fAsioTime.timeCode.timeCodeSamples.hi = 0;
		fAsioTime.timeCode.flags = kTcValid | kTcRunning ;
	} else {
		fTimeInfoMode = false;
	}

	return ASE_OK;
}

//---------------------------------------------------------------------------------------------
ASIOError JackRouter::disposeBuffers()
{
	long i;

	fCallbacks = 0;
	stop();

	for (i = 0; i < fActiveInputs; i++) {
		delete[] fInputBuffers[i];
		jack_port_unregister(fClient, fInputPorts[i]);
	}
	fActiveInputs = 0;

	for (i = 0; i < fActiveOutputs; i++) {
		delete[] fOutputBuffers[i];
		jack_port_unregister(fClient, fOutputPorts[i]);
	}
	fActiveOutputs = 0;

	return ASE_OK;
}

//---------------------------------------------------------------------------------------------
ASIOError JackRouter::controlPanel()
{
	return ASE_NotPresent;
}

//---------------------------------------------------------------------------------------------
ASIOError JackRouter::future(long selector, void* opt)	// !!! check properties
{
	ASIOTransportParameters* tp = (ASIOTransportParameters*)opt;
    
	switch (selector) {
		case kAsioEnableTimeCodeRead:	fTcRead = true;	return ASE_SUCCESS;
		case kAsioDisableTimeCodeRead:	fTcRead = false; return ASE_SUCCESS;
		case kAsioSetInputMonitor:		return ASE_SUCCESS;	// for testing!!!
		case kAsioCanInputMonitor:		return ASE_SUCCESS;	// for testing!!!
		case kAsioCanTimeInfo:			return ASE_SUCCESS;
		case kAsioCanTimeCode:			return ASE_SUCCESS;
	}
    
	return ASE_NotPresent;
}

//--------------------------------------------------------------------------------------------------------
// private methods
//--------------------------------------------------------------------------------------------------------

//---------------------------------------------------------------------------------------------
void JackRouter::bufferSwitch()
{
	if (fStarted && fCallbacks) {
		getNanoSeconds(&fTheSystemTime);			// latch system time
		fSamplePosition += fBufferSize;
		if (fTimeInfoMode) {
			bufferSwitchX ();
		} else {
			fCallbacks->bufferSwitch (fToggle, ASIOFalse);
		}
		fToggle = fToggle ? 0 : 1;
	}
}

//---------------------------------------------------------------------------------------------
// asio2 buffer switch
void JackRouter::bufferSwitchX()
{
	getSamplePosition (&fAsioTime.timeInfo.samplePosition, &fAsioTime.timeInfo.systemTime);
	long offset = fToggle ? fBufferSize : 0;
	if (fTcRead) {
		// Create a fake time code, which is 10 minutes ahead of the card's sample position
		// Please note that for simplicity here time code will wrap after 32 bit are reached
		fAsioTime.timeCode.timeCodeSamples.lo = fAsioTime.timeInfo.samplePosition.lo + 600.0 * fSampleRate;
		fAsioTime.timeCode.timeCodeSamples.hi = 0;
	}
	fCallbacks->bufferSwitchTimeInfo (&fAsioTime, fToggle, ASIOFalse);
	fAsioTime.timeInfo.flags &= ~(kSampleRateChanged | kClockSourceChanged);
}

//---------------------------------------------------------------------------------------------
ASIOError JackRouter::outputReady()
{
	return ASE_NotPresent;
}

//---------------------------------------------------------------------------------------------
double AsioSamples2double(ASIOSamples* samples)
{
	double a = (double)(samples->lo);
	if (samples->hi)
		a += (double)(samples->hi) * twoRaisedTo32;
	return a;
}

//---------------------------------------------------------------------------------------------
void getNanoSeconds(ASIOTimeStamp* ts)
{
	double nanoSeconds = (double)((unsigned long)timeGetTime ()) * 1000000.;
	ts->hi = (unsigned long)(nanoSeconds / twoRaisedTo32);
	ts->lo = (unsigned long)(nanoSeconds - (ts->hi * twoRaisedTo32));
}

//------------------------------------------------------------------------
void JackRouter::SaveConnections()
{
    const char** connections;
 	int i;

    for (i = 0; i < fActiveInputs; ++i) {
        if (fInputPorts[i] && (connections = jack_port_get_connections(fInputPorts[i])) != 0) {
            for (int j = 0; connections[j]; j++) {
                fConnections.push_back(make_pair(connections[j], jack_port_name(fInputPorts[i])));
            }
            jack_free(connections);
        }
    }

    for (i = 0; i < fActiveOutputs; ++i) {
        if (fOutputPorts[i] && (connections = jack_port_get_connections(fOutputPorts[i])) != 0) {
            for (int j = 0; connections[j]; j++) {
                fConnections.push_back(make_pair(jack_port_name(fOutputPorts[i]), connections[j]));
            }
            jack_free(connections);
        }
    }
}

//------------------------------------------------------------------------
void JackRouter::RestoreConnections()
{
    list<pair<string, string> >::const_iterator it;

    for (it = fConnections.begin(); it != fConnections.end(); it++) {
        pair<string, string> connection = *it;
        jack_connect(fClient, connection.first.c_str(), connection.second.c_str());
    }

    fConnections.clear();
}


//------------------------------------------------------------------------------------------
void JackRouter::AutoConnect()
{
	const char** ports;
	char* aliases[2];
	aliases[0] = (char*)malloc(jack_port_name_size());
	aliases[1] = (char*)malloc(jack_port_name_size());

	if (!aliases[0] || !aliases[1])
		return;

	if ((ports = jack_get_ports(fClient, NULL, NULL, JackPortIsPhysical | JackPortIsOutput)) == NULL) {
		printf("Cannot find any physical capture ports\n");
	} else {

		if (fAutoConnectIn) {
			for (int i = 0; i < fActiveInputs; i++) {
                /*
				if (!ports[i]) {
					printf("source port is null i = %ld\n", i);
					break;
				} else if (jack_connect(fClient, ports[i], jack_port_name(fInputPorts[i])) != 0) {
					printf("Cannot connect input ports\n");
				}
                */
                long ASIO_channel = fInMap[i];
                if (!ports[ASIO_channel]) {
					printf("source port is null ASIO_channel = %ld\n", ASIO_channel);
					break;
				} else if (jack_connect(fClient, ports[ASIO_channel], jack_port_name(fInputPorts[i])) != 0) {
					printf("Cannot connect input ports\n");
				} else if (fAliasSystem) {
                    jack_port_t* input_port = jack_port_by_name(fClient, ports[ASIO_channel]);
                    if (input_port) {	
                        if (jack_port_get_aliases(input_port, aliases) == 2) {
                           jack_port_set_alias(fInputPorts[i], aliases[1]);
                        }	
                    }
                }
			}
		}
		jack_free(ports);
	}

	if ((ports = jack_get_ports(fClient, NULL, NULL, JackPortIsPhysical | JackPortIsInput)) == NULL) {
		printf("Cannot find any physical playback ports");
	} else {
		if (fAutoConnectOut) {
			for (int i = 0; i < fActiveOutputs; i++) {
                /*
                if (!ports[i]){
					printf("destination port is null i = %ld\n", i);
					break;
				} else if (jack_connect(fClient, jack_port_name(fOutputPorts[i]), ports[i]) != 0) {
					printf("Cannot connect output ports\n");
				}
                */
                long ASIO_channel = fOutMap[i];
				if (!ports[ASIO_channel]) {
					printf("destination port is null ASIO_channel = %ld\n", ASIO_channel);
					break;
				} else if (jack_connect(fClient, jack_port_name(fOutputPorts[i]), ports[ASIO_channel]) != 0) {
					printf("Cannot connect output ports\n");
				} else if (fAliasSystem) {
                    jack_port_t* output_port = jack_port_by_name(fClient, ports[ASIO_channel]);
                    if (output_port) {
				        if (jack_port_get_aliases(output_port, aliases) == 2) {
                            jack_port_set_alias(fOutputPorts[i], aliases[1]);
                        }	
                    }
                }
			}
		}
		free(aliases[0]);
		free(aliases[1]);
		jack_free(ports);
	}
}

