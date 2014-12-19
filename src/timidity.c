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
 * OggVorbis source support for MultiVoc
 */

#ifdef HAVE_TIMIDITY

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <timidity.h>
#include "pitch.h"
#include "multivoc.h"
#include "_multivc.h"
#include "fx_man.h"

#define min(x,y) ((x) < (y) ? (x) : (y))
#define max(x,y) ((x) > (y) ? (x) : (y))

static playbackstatus MV_GetTimidityBlock
(
 VoiceNode *voice
 )

{
   voice->Playing = TRUE;

   MidSong *song = (MidSong*) voice->extra;

   size_t read_buf = mid_song_read_wave (song, (sint8*)voice->NextBlock, voice->BlockLength);

   if (read_buf == 0)
   {
	   if (voice->LoopStart) {
		   mid_song_start(song);
	   } else {
		  voice->Playing = FALSE;
		  return NoMoreData;
	   }
   }

   read_buf /= (voice->bits >> 3) * voice->channels;
   voice->position = 0;
   voice->sound = voice->NextBlock;
   voice->length = read_buf << 16;

   return( KeepPlaying );
}

int MV_PlayTimidity3D
(
 char *ptr,
 unsigned int ptrlength,
 int  pitchoffset,
 int  angle,
 int  distance,
 int  priority,
 unsigned int callbackval
 )

{
   int left;
   int right;
   int mid;
   int volume;
   int status;
   
   if ( !timidity_status )
   {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
   }
   
   if ( distance < 0 )
   {
      distance  = -distance;
      angle    += MV_NumPanPositions / 2;
   }
   
   volume = MIX_VOLUME( distance );
   
   // Ensure angle is within 0 - 31
   angle &= MV_MaxPanPosition;
   
   left  = MV_PanTable[ angle ][ volume ].left;
   right = MV_PanTable[ angle ][ volume ].right;
   mid   = max( 0, 255 - distance );
   
   status = MV_PlayTimidity( ptr, ptrlength, pitchoffset, mid, left, right, priority,
                           callbackval );
   
   return( status );
}

int MV_PlayTimidity
(
 char *ptr,
 unsigned int ptrlength,
 int   pitchoffset,
 int   vol,
 int   left,
 int   right,
 int   priority,
 unsigned int callbackval
 )

{
   int status;
   
   if ( !timidity_status )
   {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
   }

   status = MV_PlayLoopedTimidity( ptr, ptrlength, -1, -1, pitchoffset, vol, left, right,
                                 priority, callbackval );
   
   return( status );
}

int MV_PlayLoopedTimidity
(
 char *ptr,
 unsigned int ptrlength,
 int   loopstart,
 int   loopend,
 int   pitchoffset,
 int   vol,
 int   left,
 int   right,
 int   priority,
 unsigned int callbackval
 )

{
   VoiceNode *voice;
   MidSong *song;
   MidIStream *stream;
   int status;
   char *block;

   if ( !timidity_status )
   {
      MV_SetErrorCode( MV_NotInstalled );
      return( MV_Error );
   }

   block = malloc(options.buffer_size);
   if (!block)
   {
      return( MV_Error );
   }

   stream = mid_istream_open_mem(ptr, ptrlength, FALSE);
   song   = mid_song_load (stream, &options);
   mid_istream_close (stream);

   if (!song)
   {
	   free(block);
	   return( MV_Error );
   }

   // Request a voice from the voice pool
   voice = MV_AllocVoice( priority );

   if ( voice == NULL )
   {
	  mid_song_free (song);
      return( MV_Error );
   }

   mid_song_set_volume (song, vol >> 2);
   mid_song_start (song);

   voice->wavetype    = Timidity;
   voice->bits        = (options.format == MID_AUDIO_S16LSB) ? 16 : 8;
   voice->channels    = options.channels;
   voice->extra       = song;
   voice->GetSound    = MV_GetTimidityBlock;
   voice->NextBlock   = block;
   voice->DemandFeed  = NULL;
   voice->LoopCount   = 0;
   voice->BlockLength = options.buffer_size;
   voice->PitchScale  = PITCH_GetScale( pitchoffset );
   voice->length      = 0;
   voice->next        = NULL;
   voice->prev        = NULL;
   voice->priority    = priority;
   voice->callbackval = callbackval;
   voice->LoopStart   = (char *)(intptr_t)(loopstart >= 0 ? TRUE : FALSE);
   voice->LoopEnd     = 0;
   voice->LoopSize    = 0;
   voice->Playing     = TRUE;
   voice->Paused      = FALSE;
   
   voice->SamplingRate = options.rate;
   voice->RateScale    = ( voice->SamplingRate * voice->PitchScale ) / MV_MixRate;
   voice->FixedPointBufferSize = ( voice->RateScale * MixBufferSize ) -
      voice->RateScale;
   MV_SetVoiceMixMode( voice );

   MV_SetVoiceVolume( voice, vol, left, right );
   MV_PlayVoice( voice );
   
   return( voice->handle );
}


void MV_ReleaseTimidityVoice( VoiceNode * voice )
{
   MidSong* song = (MidSong* ) voice->extra;
   
   if (voice->wavetype != Timidity) {
	  free(voice->NextBlock);
	  mid_song_free (song);
      return;
   }
   
   voice->extra = 0;
}

#endif //HAVE_VORBIS
