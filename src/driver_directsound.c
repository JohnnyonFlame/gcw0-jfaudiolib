/*
 Copyright (C) 2009 Jonathon Fowler <jf@jonof.id.au>
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 
 See the GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 
 */

/**
 * DirectSound output driver for MultiVoc
 */

#define WIN32_LEAN_AND_MEAN
#define DIRECTSOUND_VERSION  0x0700
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdlib.h>
#include <stdio.h>

#include "driver_directsound.h"

enum {
   DSErr_Warning = -2,
   DSErr_Error   = -1,
   DSErr_Ok      = 0,
	DSErr_Uninitialised,
	DSErr_DirectSoundCreate,
	DSErr_SetCooperativeLevel,
	DSErr_CreateSoundBuffer,
	DSErr_CreateSoundBufferSecondary,
	DSErr_SetFormat,
	DSErr_SetFormatSecondary,
	DSErr_Notify,
	DSErr_NotifyEvents,
	DSErr_SetNotificationPositions,
	DSErr_Play,
	DSErr_PlaySecondary,
	DSErr_CreateThread,
	DSErr_CreateMutex,
    DSErr_NotifyWindow,
    DSErr_CDMCIOpen,
    DSErr_CDMCISetTimeFormat,
    DSErr_CDMCIPlay
};

static int ErrorCode = DSErr_Ok;
static int Initialised = 0;
static int Playing = 0;

static char *MixBuffer = 0;
static int MixBufferSize = 0;
static int MixBufferCount = 0;
static int MixBufferCurrent = 0;
static int MixBufferUsed = 0;
static void ( *MixCallBack )( void ) = 0;

static LPDIRECTSOUND lpds = 0;
static LPDIRECTSOUNDBUFFER lpdsbprimary = 0, lpdsbsec = 0;
static LPDIRECTSOUNDNOTIFY lpdsnotify = 0;
static DSBPOSITIONNOTIFY notifyPositions[3] = { { 0,0 }, { 0,0 }, { 0,0 } };
static HANDLE mixThread = 0;
static HANDLE mutex = 0;

enum {
    UsedByNothing = 0,
    UsedByMIDI = 1,
    UsedByCD = 2
};

static HWND notifyWindow = 0;
static int notifyWindowClassRegistered = 0;
static int notifyWindowUsedBy = UsedByNothing;

static UINT cdDeviceID = 0;
static DWORD cdPausePosition = 0;
static int cdPaused = 0;
static int cdLoop = 0;
static int cdPlayTrack = 0;


static void FillBufferPortion(char * ptr, int remaining)
{
    int len;
	char *sptr;

	while (remaining > 0) {
		if (MixBufferUsed == MixBufferSize) {
			MixCallBack();
			
			MixBufferUsed = 0;
			MixBufferCurrent++;
			if (MixBufferCurrent >= MixBufferCount) {
				MixBufferCurrent -= MixBufferCount;
			}
		}
		
		while (remaining > 0 && MixBufferUsed < MixBufferSize) {
			sptr = MixBuffer + (MixBufferCurrent * MixBufferSize) + MixBufferUsed;
			
			len = MixBufferSize - MixBufferUsed;
			if (remaining < len) {
				len = remaining;
			}
			
			memcpy(ptr, sptr, len);
			
			ptr += len;
			MixBufferUsed += len;
			remaining -= len;
		}
	}
}

static void FillBuffer(int bufnum)
{
    HRESULT err;
    LPVOID ptr, ptr2;
    DWORD remaining, remaining2;
    int retries = 1;
    
    //fprintf(stderr, "DirectSound FillBuffer: filling %d\n", bufnum);

    do {
        err = IDirectSoundBuffer_Lock(lpdsbsec,
                  notifyPositions[bufnum].dwOffset,
                  notifyPositions[1].dwOffset,
                  &ptr, &remaining,
                  &ptr2, &remaining2,
                  0);
        if (FAILED(err)) {
            if (err == DSERR_BUFFERLOST) {
                err = IDirectSoundBuffer_Restore(lpdsbsec);
                if (FAILED(err)) {
                    return;
                }

                if (retries-- > 0) {
                    continue;
                }
            }
            fprintf(stderr, "DirectSound FillBuffer: err %x\n", (unsigned int) err);
            return;
        }
        break;
    } while (1);
    
    if (ptr) {
        FillBufferPortion((char *) ptr, remaining);
    }
    if (ptr2) {
        FillBufferPortion((char *) ptr2, remaining2);
    }
    
    IDirectSoundBuffer_Unlock(lpdsbsec, ptr, remaining, ptr2, remaining2);
}

static DWORD WINAPI fillDataThread(LPVOID lpParameter)
{
    HANDLE handles[3];
    DWORD waitret, waitret2;
    
    handles[0] = notifyPositions[0].hEventNotify;
    handles[1] = notifyPositions[1].hEventNotify;
    handles[2] = notifyPositions[2].hEventNotify;
    
	do {
        waitret = WaitForMultipleObjects(3, handles, FALSE, INFINITE); 
        switch (waitret) {
            case WAIT_OBJECT_0:
            case WAIT_OBJECT_0+1:
                waitret2 = WaitForSingleObject(mutex, INFINITE);
                if (waitret2 == WAIT_OBJECT_0) {
                    FillBuffer(WAIT_OBJECT_0 + 1 - waitret);
                    ReleaseMutex(mutex);
                } else {
                    fprintf(stderr, "DirectSound fillDataThread: wfso err %d\n", (int) waitret2);
                }
                break;
            case WAIT_OBJECT_0+2:
                fprintf(stderr, "DirectSound fillDataThread: exiting\n");
                ExitThread(0);
                break;
            default:
                fprintf(stderr, "DirectSound fillDataThread: wfmo err %d\n", (int) waitret);
                break;
        }
	} while (1);
	
	return 0;
}


int DirectSoundDrv_GetError(void)
{
	return ErrorCode;
}

const char *DirectSoundDrv_ErrorString( int ErrorNumber )
{
	const char *ErrorString;
	
   switch( ErrorNumber )
	{
      case DSErr_Warning :
      case DSErr_Error :
         ErrorString = DirectSoundDrv_ErrorString( ErrorCode );
         break;
			
      case DSErr_Ok :
         ErrorString = "DirectSound ok.";
         break;
			
		case DSErr_Uninitialised:
			ErrorString = "DirectSound uninitialised.";
			break;
			
		case DSErr_DirectSoundCreate:
            ErrorString = "DirectSound error: DirectSoundCreate failed.";
            break;
            
        case DSErr_SetCooperativeLevel:
            ErrorString = "DirectSound error: SetCooperativeLevel failed.";
            break;
            
        case DSErr_CreateSoundBuffer:
            ErrorString = "DirectSound error: primary CreateSoundBuffer failed.";
            break;
        
        case DSErr_CreateSoundBufferSecondary:
            ErrorString = "DirectSound error: secondary CreateSoundBuffer failed.";
            break;
        
        case DSErr_SetFormat:
            ErrorString = "DirectSound error: primary buffer SetFormat failed.";
            break;
			
        case DSErr_SetFormatSecondary:
            ErrorString = "DirectSound error: secondary buffer SetFormat failed.";
            break;
            
        case DSErr_Notify:
            ErrorString = "DirectSound error: failed querying secondary buffer for notify interface.";
            break;
            
        case DSErr_NotifyEvents:
            ErrorString = "DirectSound error: failed creating notify events.";
            break;
        
        case DSErr_SetNotificationPositions:
            ErrorString = "DirectSound error: failed setting notification positions.";
            break;
            
        case DSErr_Play:
            ErrorString = "DirectSound error: primary buffer Play failed.";
            break;
            
        case DSErr_PlaySecondary:
            ErrorString = "DirectSound error: secondary buffer Play failed.";
            break;
        
        case DSErr_CreateThread:
            ErrorString = "DirectSound error: failed creating mix thread.";
            break;
        
        case DSErr_CreateMutex:
            ErrorString = "DirectSound error: failed creating mix mutex.";
            break;

        case DSErr_NotifyWindow:
            ErrorString = "Failed creating notification window for CD/MIDI.";
            break;

        case DSErr_CDMCIOpen:
            ErrorString = "MCI error: failed opening CD audio device.";
            break;

        case DSErr_CDMCISetTimeFormat:
            ErrorString = "MCI error: failed setting time format for CD audio device.";
            break;

        case DSErr_CDMCIPlay:
            ErrorString = "MCI error: failed playing CD audio track.";
            break;

		default:
			ErrorString = "Unknown DirectSound error code.";
			break;
	}
	
	return ErrorString;

}


static void TeardownDSound(HRESULT err)
{
    if (FAILED(err)) {
        fprintf(stderr, "Dying error: %x\n", (unsigned int) err);
    }

    if (lpdsnotify)   IDirectSoundNotify_Release(lpdsnotify);
    if (notifyPositions[0].hEventNotify) CloseHandle(notifyPositions[0].hEventNotify);
    if (notifyPositions[1].hEventNotify) CloseHandle(notifyPositions[1].hEventNotify);
    if (notifyPositions[2].hEventNotify) CloseHandle(notifyPositions[2].hEventNotify);
    if (mutex) CloseHandle(mutex);
    if (lpdsbsec)     IDirectSoundBuffer_Release(lpdsbsec);
    if (lpdsbprimary) IDirectSoundBuffer_Release(lpdsbprimary);
    if (lpds)         IDirectSound_Release(lpds);
    notifyPositions[0].hEventNotify =
    notifyPositions[1].hEventNotify =
    notifyPositions[2].hEventNotify = 0;
    mutex = 0;
    lpdsnotify = 0;
    lpdsbsec = 0;
    lpdsbprimary = 0;
    lpds = 0;
}

int DirectSoundDrv_PCM_Init(int * mixrate, int * numchannels, int * samplebits, void * initdata)
{
    HRESULT err;
    DSBUFFERDESC bufdesc;
    WAVEFORMATEX wfex;
    
    if (Initialised) {
        DirectSoundDrv_PCM_Shutdown();
    }
    
    err = DirectSoundCreate(0, &lpds, 0);
    if (FAILED( err )) {
        ErrorCode = DSErr_DirectSoundCreate;
        return DSErr_Error;
    }
    
    err = IDirectSound_SetCooperativeLevel(lpds, (HWND) initdata, DSSCL_PRIORITY);
    if (FAILED( err )) {
        TeardownDSound(err);
        ErrorCode = DSErr_SetCooperativeLevel;
        return DSErr_Error;
    }
    
    memset(&bufdesc, 0, sizeof(DSBUFFERDESC));
    bufdesc.dwSize = sizeof(DSBUFFERDESC);
    bufdesc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    
    err = IDirectSound_CreateSoundBuffer(lpds, &bufdesc, &lpdsbprimary, 0);
    if (FAILED( err )) {
        TeardownDSound(err);
        ErrorCode = DSErr_CreateSoundBuffer;
        return DSErr_Error;
    }
    
    memset(&wfex, 0, sizeof(WAVEFORMATEX));
    wfex.wFormatTag = WAVE_FORMAT_PCM;
    wfex.nChannels = *numchannels;
    wfex.nSamplesPerSec = *mixrate;
    wfex.wBitsPerSample = *samplebits;
    wfex.nBlockAlign = wfex.nChannels * wfex.wBitsPerSample / 8;
    wfex.nAvgBytesPerSec = wfex.nSamplesPerSec * wfex.nBlockAlign;
    
    err = IDirectSoundBuffer_SetFormat(lpdsbprimary, &wfex);
    if (FAILED( err )) {
        TeardownDSound(err);
        ErrorCode = DSErr_SetFormat;
        return DSErr_Error;
    }
    
    bufdesc.dwFlags = DSBCAPS_LOCSOFTWARE |
                      DSBCAPS_CTRLPOSITIONNOTIFY |
                      DSBCAPS_GETCURRENTPOSITION2;
    bufdesc.dwBufferBytes = wfex.nBlockAlign * 2048 * 2;
    bufdesc.lpwfxFormat = &wfex;
    
    err = IDirectSound_CreateSoundBuffer(lpds, &bufdesc, &lpdsbsec, 0);
    if (FAILED( err )) {
        TeardownDSound(err);
        ErrorCode = DSErr_SetFormatSecondary;
        return DSErr_Error;
    }
    
    err = IDirectSoundBuffer_QueryInterface(lpdsbsec, &IID_IDirectSoundNotify,
            (LPVOID *) &lpdsnotify);
    if (FAILED( err )) {
        TeardownDSound(err);
        ErrorCode = DSErr_Notify;
        return DSErr_Error;
    }
    
    notifyPositions[0].dwOffset = 0;
    notifyPositions[0].hEventNotify = CreateEvent(NULL, FALSE, FALSE, NULL);
    notifyPositions[1].dwOffset = bufdesc.dwBufferBytes / 2;
    notifyPositions[1].hEventNotify = CreateEvent(NULL, FALSE, FALSE, NULL);
    notifyPositions[2].dwOffset = DSBPN_OFFSETSTOP;
    notifyPositions[2].hEventNotify = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!notifyPositions[0].hEventNotify ||
        !notifyPositions[1].hEventNotify ||
        !notifyPositions[2].hEventNotify) {
        TeardownDSound(DS_OK);
        ErrorCode = DSErr_NotifyEvents;
        return DSErr_Error;
    }
    
    err = IDirectSoundNotify_SetNotificationPositions(lpdsnotify, 3, notifyPositions);
    if (FAILED( err )) {
        TeardownDSound(err);
        ErrorCode = DSErr_SetNotificationPositions;
        return DSErr_Error;
    }
    
    err = IDirectSoundBuffer_Play(lpdsbprimary, 0, 0, DSBPLAY_LOOPING);
    if (FAILED( err )) {
        TeardownDSound(err);
        ErrorCode = DSErr_Play;
        return DSErr_Error;
    }
    
    mutex = CreateMutex(0, FALSE, 0);
    if (!mutex) {
        TeardownDSound(DS_OK);
        ErrorCode = DSErr_CreateMutex;
        return DSErr_Error;
    }
    
    Initialised = 1;
    
    fprintf(stderr, "DirectSound Init: yay\n");
    
	return DSErr_Ok;
}

void DirectSoundDrv_PCM_Shutdown(void)
{
    if (!Initialised) {
        return;
    }
    
    DirectSoundDrv_PCM_StopPlayback();
    
    TeardownDSound(DS_OK);
    
    Initialised = 0;
}

int DirectSoundDrv_PCM_BeginPlayback(char *BufferStart, int BufferSize,
						int NumDivisions, void ( *CallBackFunc )( void ) )
{
    HRESULT err;
    
    if (!Initialised) {
        ErrorCode = DSErr_Uninitialised;
        return DSErr_Error;
    }
    
    DirectSoundDrv_PCM_StopPlayback();
    
	MixBuffer = BufferStart;
	MixBufferSize = BufferSize;
	MixBufferCount = NumDivisions;
	MixBufferCurrent = 0;
	MixBufferUsed = 0;
	MixCallBack = CallBackFunc;

	// prime the buffer
	FillBuffer(0);
	
	mixThread = CreateThread(NULL, 0, fillDataThread, 0, 0, 0);
	if (!mixThread) {
        ErrorCode = DSErr_CreateThread;
        return DSErr_Error;
    }

    SetThreadPriority(mixThread, THREAD_PRIORITY_HIGHEST);
    
    err = IDirectSoundBuffer_Play(lpdsbsec, 0, 0, DSBPLAY_LOOPING);
    if (FAILED( err )) {
        ErrorCode = DSErr_PlaySecondary;
        return DSErr_Error;
    }
    
    Playing = 1;
    
	return DSErr_Ok;
}

void DirectSoundDrv_PCM_StopPlayback(void)
{
    if (!Playing) {
        return;
    }
    
    IDirectSoundBuffer_Stop(lpdsbsec);
    IDirectSoundBuffer_SetCurrentPosition(lpdsbsec, 0);
    
    Playing = 0;
}

void DirectSoundDrv_PCM_Lock(void)
{
    DWORD err;
    
    err = WaitForSingleObject(mutex, INFINITE);
    if (err != WAIT_OBJECT_0) {
        fprintf(stderr, "DirectSound lock: wfso %d\n", (int) err);
    }
}

void DirectSoundDrv_PCM_Unlock(void)
{
    ReleaseMutex(mutex);
}



static LRESULT CALLBACK notifyWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
        case MM_MCINOTIFY:
            if (wParam == MCI_NOTIFY_SUCCESSFUL && lParam == cdDeviceID) {
                if (cdLoop && cdPlayTrack) {
                    DirectSoundDrv_CD_Play(cdPlayTrack, 1);
                }
            }
            break;
        default: break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

static int openNotifyWindow(int useby)
{
    if (!notifyWindow) {
        if (!notifyWindowClassRegistered) {
            WNDCLASS wc;

            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = notifyWindowProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = "JFAudiolibNotifyWindow";

            if (!RegisterClass(&wc)) {
                return 0;
            }

            notifyWindowClassRegistered = 1;
        }

        notifyWindow = CreateWindow("JFAudiolibNotifyWindow", "", WS_POPUP,
                0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);
        if (!notifyWindow) {
            return 0;
        }
    }

    notifyWindowUsedBy |= useby;

    return 1;
}

static void closeNotifyWindow(int useby)
{
    notifyWindowUsedBy &= ~useby;

    if (!notifyWindowUsedBy && notifyWindow) {
        DestroyWindow(notifyWindow);
        notifyWindow = 0;
    }
}


int DirectSoundDrv_CD_Init(void)
{
    MCI_OPEN_PARMS mciopenparms;
    MCI_SET_PARMS mcisetparms;
    DWORD rv;

    DirectSoundDrv_CD_Shutdown();

    mciopenparms.lpstrDeviceType = "cdaudio";
    rv = mciSendCommand(0, MCI_OPEN, MCI_OPEN_TYPE, (DWORD)(LPVOID) &mciopenparms);
    if (rv) {
        fprintf(stderr, "Init MCI_OPEN err %d\n", (int) rv);
        ErrorCode = DSErr_CDMCIOpen;
        return DSErr_Error;
    }

    cdDeviceID = mciopenparms.wDeviceID;

    mcisetparms.dwTimeFormat = MCI_FORMAT_TMSF;
    rv = mciSendCommand(cdDeviceID, MCI_SET, MCI_SET_TIME_FORMAT, (DWORD)(LPVOID) &mcisetparms);
    if (rv) {
        fprintf(stderr, "Init MCI_SET err %d\n", (int) rv);
        mciSendCommand(cdDeviceID, MCI_CLOSE, 0, 0);
        cdDeviceID = 0;

        ErrorCode = DSErr_CDMCISetTimeFormat;
        return DSErr_Error;
    }

    if (!openNotifyWindow(UsedByCD)) {
        mciSendCommand(cdDeviceID, MCI_CLOSE, 0, 0);
        cdDeviceID = 0;

        ErrorCode = DSErr_NotifyWindow;
        return DSErr_Error;
    }

    return DSErr_Ok;
}

void DirectSoundDrv_CD_Shutdown(void)
{
    if (cdDeviceID) {
        DirectSoundDrv_CD_Stop();

        mciSendCommand(cdDeviceID, MCI_CLOSE, 0, 0);
    }
    cdDeviceID = 0;

    closeNotifyWindow(UsedByCD);
}

int DirectSoundDrv_CD_Play(int track, int loop)
{
    MCI_PLAY_PARMS mciplayparms;
    DWORD rv;

    if (!cdDeviceID) {
        ErrorCode = DSErr_Uninitialised;
        return DSErr_Error;
    }

    cdPlayTrack = track;
    cdLoop = loop;
    cdPaused = 0;

    mciplayparms.dwFrom = MCI_MAKE_TMSF(track, 0, 0, 0);
    mciplayparms.dwTo   = MCI_MAKE_TMSF(track + 1, 0, 0, 0);
    mciplayparms.dwCallback = (DWORD) notifyWindow;
    rv = mciSendCommand(cdDeviceID, MCI_PLAY, MCI_FROM | MCI_TO | MCI_NOTIFY, (DWORD)(LPVOID) &mciplayparms);
    if (rv) {
        fprintf(stderr, "Play MCI_PLAY err %d\n", (int) rv);
        ErrorCode = DSErr_CDMCIPlay;
        return DSErr_Error;
    }

    return DSErr_Ok;
}

void DirectSoundDrv_CD_Stop(void)
{
    MCI_GENERIC_PARMS mcigenparms;
    DWORD rv;

    if (!cdDeviceID) {
        return;
    }

    cdPlayTrack = 0;
    cdLoop = 0;
    cdPaused = 0;

    rv = mciSendCommand(cdDeviceID, MCI_STOP, 0, (DWORD)(LPVOID) &mcigenparms);
    if (rv) {
        fprintf(stderr, "Stop MCI_STOP err %d\n", (int) rv);
    }
}

void DirectSoundDrv_CD_Pause(int pauseon)
{
    if (!cdDeviceID) {
        return;
    }

    if (cdPaused == pauseon) {
        return;
    }

    if (pauseon) {
        MCI_STATUS_PARMS mcistatusparms;
        MCI_GENERIC_PARMS mcigenparms;
        DWORD rv;

        mcistatusparms.dwItem = MCI_STATUS_POSITION;
        rv = mciSendCommand(cdDeviceID, MCI_STATUS, MCI_WAIT | MCI_STATUS_ITEM, (DWORD)(LPVOID) &mcistatusparms);
        if (rv) {
            fprintf(stderr, "Pause MCI_STATUS err %d\n", (int) rv);
            return;
        }

        cdPausePosition = mcistatusparms.dwReturn;

        rv = mciSendCommand(cdDeviceID, MCI_STOP, 0, (DWORD)(LPVOID) &mcigenparms);
        if (rv) {
            fprintf(stderr, "Pause MCI_STOP err %d\n", (int) rv);
        }
    } else {
        MCI_PLAY_PARMS mciplayparms;
        DWORD rv;

        mciplayparms.dwFrom = cdPausePosition;
        mciplayparms.dwTo   = MCI_MAKE_TMSF(cdPlayTrack + 1, 0, 0, 0);
        mciplayparms.dwCallback = (DWORD) notifyWindow;
        rv = mciSendCommand(cdDeviceID, MCI_PLAY, MCI_FROM | MCI_TO | MCI_NOTIFY, (DWORD)(LPVOID) &mciplayparms);
        if (rv) {
            fprintf(stderr, "Pause MCI_PLAY err %d\n", (int) rv);
            return;
        }

        cdPausePosition = 0;
    }

    cdPaused = pauseon;
}

int DirectSoundDrv_CD_IsPlaying(void)
{
    MCI_STATUS_PARMS mcistatusparms;
    DWORD rv;

    if (!cdDeviceID) {
        return 0;
    }

    mcistatusparms.dwItem = MCI_STATUS_MODE;
    rv = mciSendCommand(cdDeviceID, MCI_STATUS, MCI_WAIT | MCI_STATUS_ITEM, (DWORD)(LPVOID) &mcistatusparms);
    if (rv) {
        fprintf(stderr, "IsPlaying MCI_STATUS err %d\n", (int) rv);
        return 0;
    }

    return (mcistatusparms.dwReturn == MCI_MODE_PLAY);
}

void DirectSoundDrv_CD_SetVolume(int volume)
{
}


// vim:ts=4:sw=4:expandtab:
